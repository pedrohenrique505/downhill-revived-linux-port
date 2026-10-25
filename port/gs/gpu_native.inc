/* The native renderer's Direct3D 11 backend (DHD_RENDER=native; the renderer itself, portable, is
   native.c). Each surface is a texture with a render target or depth view, drawn with the pipeline
   of raster.hlsl's native_* shaders; textures decoded on the GS worker become immutable textures;
   the displayed picture is scanned out by a compute shader and drawn into the swap chain's back
   buffer. Included by gpu.c, whose exact backend runs on a device of its own (gs_gpu_init). */

#include "native.h"
#include "font_sharp.h"
#include <math.h>

void pacing_present(double due, double began);  /* port/hle/kernel.c, just-in-time frames */

/* The Direct3D 11 backend: the device, its pipeline, and each surface's texture and views. */
typedef struct {
    ID3D11Texture2D *texture;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;
} NativeD3dSurface;
typedef struct {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    NativeD3dSurface surfaces[NATIVE_SURFACES];
    ID3D11SamplerState *replace_samplers[8];  /* render thread: bilinear | U clamped << 1 | V clamped << 2 */
    IDXGISwapChain *swapchain;
    ID3D11Texture2D *backbuffer;
    ID3D11RenderTargetView *back_view;
    unsigned buffer_width, buffer_height;
    HWND window;
    ID3D11VertexShader *present_vertex;
    ID3D11PixelShader *present_pixel;
    ID3D11SamplerState *present_sampler;
    ID3D11VertexShader *vertex_shader;
    ID3D11PixelShader *pixel_shader;
    ID3D11ComputeShader *scanout_shader, *convert_shader;
    ID3D11Buffer *convert_constants, *inverse;
    ID3D11ShaderResourceView *inverse_view;
    struct { ID3D11Texture2D *texture; ID3D11ShaderResourceView *srv; ID3D11UnorderedAccessView *uav; unsigned w, h; }
        converted[NATIVE_CONVERTS];
    ID3D11InputLayout *layout;
    ID3D11Buffer *vertex_buffer, *constant_buffer, *display_constants;
    unsigned vertex_buffer_capacity;
    ID3D11RasterizerState *rasterizer;
    struct { uint32_t key; ID3D11BlendState *state; } blends[128];
    unsigned blend_count;
    ID3D11DepthStencilState *depth_states[16];
    ID3D11PixelShader *blit_pixel;
    ID3D11PixelShader *alpha_upload_pixel;
    ID3D11Buffer *blit_constants;
    ID3D11Texture2D *import;           /* a 1x page, drawn into an upscaled surface */
    ID3D11ShaderResourceView *import_view;
    NativeConvertConstants convert_scratch;
    ID3D11Texture2D *scratch, *display, *display_readback;
    ID3D11ShaderResourceView *scratch_view;
    unsigned scratch_size[2];
    ID3D11UnorderedAccessView *display_uav;
    ID3D11ShaderResourceView *display_view;
    struct { uint64_t self_copies, self_copy_px, copies, copy_px, imports, import_px, draws; } drawn;
    struct {
        ID3D11Query *disjoint, *begin, *end;
        int pending; uint64_t field; float t, cpu_ms;
        uint64_t self_copies, self_copy_px, copies, copy_px, imports, import_px, draws;
    } gpu_time[8];
    unsigned gpu_time_next;
} NativeD3d;

static NativeD3dSurface *d3d_surface(Native *n, const NativeSurface *s)
{
    return &((NativeD3d *)n->gpu)->surfaces[s - n->surfaces];
}

/* ---- surfaces and drawing (render thread, context lock held) ---- */

static ID3D11BlendState *native_blend_state(Native *n, uint32_t key, unsigned mask)
{
    NativeD3d *d3d = n->gpu;
    uint32_t full = key | mask << 16;
    for (unsigned i = 0; i < d3d->blend_count; i++) if (d3d->blends[i].key == full) return d3d->blends[i].state;
    D3D11_BLEND_DESC d = {0};
    D3D11_RENDER_TARGET_BLEND_DESC *t = &d.RenderTarget[0];
    t->BlendEnable = key & 1;
    /* As comes from the shader's second output (As / 128; the target keeps alpha / 255). */
    static const D3D11_BLEND dual[32] = {[BLEND_SA] = D3D11_BLEND_SRC1_ALPHA, [BLEND_ISA] = D3D11_BLEND_INV_SRC1_ALPHA};
    unsigned src = key >> 1 & 31, dst = key >> 6 & 31;
    t->SrcBlend = key ? (dual[src] ? dual[src] : (D3D11_BLEND)src) : D3D11_BLEND_ONE;
    t->DestBlend = key ? (dual[dst] ? dual[dst] : (D3D11_BLEND)dst) : D3D11_BLEND_ZERO;
    t->BlendOp = key ? (D3D11_BLEND_OP)(key >> 11 & 7) : D3D11_BLEND_OP_ADD;
    t->SrcBlendAlpha = D3D11_BLEND_ONE; t->DestBlendAlpha = D3D11_BLEND_ZERO; t->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    t->RenderTargetWriteMask = (UINT8)mask;
    ID3D11BlendState *state = NULL;
    if (FAILED(ID3D11Device_CreateBlendState(d3d->device, &d, &state))) return NULL;
    if (d3d->blend_count < sizeof d3d->blends / sizeof *d3d->blends)
        d3d->blends[d3d->blend_count].key = full, d3d->blends[d3d->blend_count++].state = state;
    return state;
}

static ID3D11DepthStencilState *native_depth_state(Native *n, unsigned key)
{
    NativeD3d *d3d = n->gpu;
    if (!d3d->depth_states[key & 15]) {
        static const D3D11_COMPARISON_FUNC func[4] = {D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS,
                                                       D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_GREATER};
        D3D11_DEPTH_STENCIL_DESC d = {0};
        d.DepthEnable = (key & 8) != 0;
        d.DepthWriteMask = key & 4 ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = func[key & 3];
        ID3D11Device_CreateDepthStencilState(d3d->device, &d, &d3d->depth_states[key & 15]);
    }
    return d3d->depth_states[key & 15];
}

static void native_release_surface(NativeD3dSurface *s)
{
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->dsv) ID3D11DepthStencilView_Release(s->dsv);
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->texture) ID3D11Texture2D_Release(s->texture);
    s->rtv = NULL; s->dsv = NULL; s->srv = NULL; s->texture = NULL;
}

/* Creates the surface's texture at its scale, or a larger one keeping what it holds. */
int native_gpu_surface(Native *n, NativeSurface *s, const unsigned want[2])
{
    NativeD3d *d3d = n->gpu;
    NativeD3dSurface *r = d3d_surface(n, s);
    unsigned w = want[0] * s->scale, h = want[1] * s->scale;
    if (r->texture && s->size[0] >= w && s->size[1] >= h) return 1;
    NativeD3dSurface old = *r;
    if (old.texture) {
        if (w < s->size[0]) w = s->size[0];
        if (h < s->size[1]) h = s->size[1];
    }
    r->texture = NULL; r->rtv = NULL; r->dsv = NULL; r->srv = NULL;
    D3D11_TEXTURE2D_DESC d = {0};
    d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = 1; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    int ok;
    if (s->depth) {
        /* Typeless, so a conversion can read the Z buffer as a texture (the depth haze). */
        d.Format = DXGI_FORMAT_R32_TYPELESS; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2D};
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D};
        sv.Texture2D.MipLevels = 1;
        ok = SUCCEEDED(ID3D11Device_CreateTexture2D(d3d->device, &d, NULL, &r->texture)) &&
             SUCCEEDED(ID3D11Device_CreateDepthStencilView(d3d->device, (ID3D11Resource *)r->texture, &dv, &r->dsv)) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(d3d->device, (ID3D11Resource *)r->texture, &sv, &r->srv));
        if (ok) ID3D11DeviceContext_ClearDepthStencilView(d3d->context, r->dsv, D3D11_CLEAR_DEPTH, 0, 0);
    } else {
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ok = SUCCEEDED(ID3D11Device_CreateTexture2D(d3d->device, &d, NULL, &r->texture)) &&
             SUCCEEDED(ID3D11Device_CreateRenderTargetView(d3d->device, (ID3D11Resource *)r->texture, NULL, &r->rtv)) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(d3d->device, (ID3D11Resource *)r->texture, NULL, &r->srv));
        const float zero[4] = {0, 0, 0, 0};
        if (ok) ID3D11DeviceContext_ClearRenderTargetView(d3d->context, r->rtv, zero);
    }
    if (!ok) {
        native_release_surface(r);
        *r = old;
        return old.texture != NULL;
    }
    s->size[0] = w; s->size[1] = h;
    if (old.texture) {
        ID3D11DeviceContext_CopySubresourceRegion(d3d->context, (ID3D11Resource *)r->texture, 0, 0, 0, 0,
                                                  (ID3D11Resource *)old.texture, 0, NULL);
        native_release_surface(&old);
    }
    return 1;
}

/* The draw state native_render keeps, again after a blit used the pipeline. */
static void native_bind_pipeline(Native *n)
{
    NativeD3d *d3d = n->gpu;
    ID3D11DeviceContext *c = d3d->context;
    UINT stride = sizeof(NativeVertex), zero = 0;
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &d3d->vertex_buffer, &stride, &zero);
    ID3D11DeviceContext_IASetInputLayout(c, d3d->layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, d3d->vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, d3d->pixel_shader, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(c, 3, 1, &d3d->constant_buffer);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 3, 1, &d3d->constant_buffer);
    ID3D11DeviceContext_RSSetState(c, d3d->rasterizer);
}

/* A w x h rectangle (GS pixels) of a texture at scale `from`, at (sx, sy), into surface t at
   (x, y), when the scales differ (raster.hlsl native_blit). Leaves no target bound. */
static void native_blit(Native *n, ID3D11ShaderResourceView *source, unsigned from, int sx, int sy, NativeSurface *t,
                        int x, int y, int w, int h)
{
    NativeD3d *d3d = n->gpu;
    ID3D11DeviceContext *c = d3d->context;
    ID3D11RenderTargetView *rtv = d3d_surface(n, t)->rtv;
    unsigned T = t->scale;
    if (x + w > (int)(t->size[0] / T)) w = (int)(t->size[0] / T) - x;
    if (y + h > (int)(t->size[1] / T)) h = (int)(t->size[1] / T) - y;
    if (!rtv || !d3d->blit_pixel || w <= 0 || h <= 0) return;
    float map[4] = {(float)(sx - x), (float)(sy - y), (float)T, (float)from};
    D3D11_VIEWPORT viewport = {(float)(x * (int)T), (float)(y * (int)T), (float)(w * (int)T), (float)(h * (int)T), 0, 1};
    D3D11_RECT rect = {x * (LONG)T, y * (LONG)T, (x + w) * (LONG)T, (y + h) * (LONG)T};
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)d3d->blit_constants, 0, NULL, map, 0, 0);
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(c, NULL, NULL, ~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(c, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_VSSetShader(c, d3d->present_vertex, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, d3d->blit_pixel, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 5, 1, &d3d->blit_constants);
    ID3D11DeviceContext_PSSetShaderResources(c, 7, 1, &source);
    ID3D11DeviceContext_RSSetViewports(c, 1, &viewport);
    ID3D11DeviceContext_RSSetScissorRects(c, 1, &rect);
    ID3D11DeviceContext_Draw(c, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(c, 7, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    native_bind_pipeline(n);
}

/* The scratch texture holds at least w x h pixels (a copy of a region a draw samples). */
static int native_scratch(Native *n, unsigned w, unsigned h)
{
    NativeD3d *d3d = n->gpu;
    if (d3d->scratch && d3d->scratch_size[0] >= w && d3d->scratch_size[1] >= h) return 1;
    if (d3d->scratch_view) ID3D11ShaderResourceView_Release(d3d->scratch_view);
    if (d3d->scratch) ID3D11Texture2D_Release(d3d->scratch);
    d3d->scratch = NULL; d3d->scratch_view = NULL;
    if (w < d3d->scratch_size[0]) w = d3d->scratch_size[0];
    if (h < d3d->scratch_size[1]) h = d3d->scratch_size[1];
    D3D11_TEXTURE2D_DESC t = {0};
    t.Width = w; t.Height = h; t.MipLevels = t.ArraySize = 1; t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(d3d->device, &t, NULL, &d3d->scratch)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(d3d->device, (ID3D11Resource *)d3d->scratch, NULL, &d3d->scratch_view))) {
        if (d3d->scratch) ID3D11Texture2D_Release(d3d->scratch);
        d3d->scratch = NULL; d3d->scratch_size[0] = d3d->scratch_size[1] = 0;
        return 0;
    }
    d3d->scratch_size[0] = w; d3d->scratch_size[1] = h;
    return 1;
}

/* A w x h rectangle (GS pixels) of surface s at (sx, sy) to surface t at (x, y). */
static void native_copy(Native *n, NativeSurface *s, NativeSurface *t, int sx, int sy, int x, int y, int w, int h)
{
    NativeD3d *d3d = n->gpu;
    ID3D11DeviceContext *c = d3d->context;
    ID3D11Resource *from = (ID3D11Resource *)d3d_surface(n, s)->texture, *to = (ID3D11Resource *)d3d_surface(n, t)->texture;
    if (s->scale != t->scale) { native_blit(n, d3d_surface(n, s)->srv, s->scale, sx, sy, t, x, y, w, h); return; }
    int S = (int)s->scale;
    if (sx + w > (int)s->size[0] / S) w = (int)s->size[0] / S - sx;
    if (sy + h > (int)s->size[1] / S) h = (int)s->size[1] / S - sy;
    if (x + w > (int)t->size[0] / S) w = (int)t->size[0] / S - x;
    if (y + h > (int)t->size[1] / S) h = (int)t->size[1] / S - y;
    if (w <= 0 || h <= 0) return;
    D3D11_BOX box = {(UINT)(sx * S), (UINT)(sy * S), 0, (UINT)((sx + w) * S), (UINT)((sy + h) * S), 1};
    d3d->drawn.copies++; d3d->drawn.copy_px += (uint64_t)w * h;
    if (s == t) {
        if (!native_scratch(n, box.right, box.bottom)) return;
        ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)d3d->scratch, 0, box.left, box.top, 0, from, 0, &box);
        ID3D11DeviceContext_CopySubresourceRegion(c, to, 0, (UINT)(x * S), (UINT)(y * S), 0, (ID3D11Resource *)d3d->scratch, 0, &box);
    } else
        ID3D11DeviceContext_CopySubresourceRegion(c, to, 0, (UINT)(x * S), (UINT)(y * S), 0, from, 0, &box);
}

/* Apply a page of masked alpha bits without resampling its existing scaled RGB. */
static void native_alpha_upload(Native *n, const NativeCommand *k)
{
    NativeD3d *d3d = n->gpu;
    NativeSurface *s = &n->surfaces[k->target];
    NativeD3dSurface *r = d3d_surface(n, s);
    ID3D11DeviceContext *c = d3d->context;
    unsigned S = s->scale;
    D3D11_BOX box = {(UINT)k->x * S, (UINT)k->y * S, 0,
                    (UINT)(k->x + 64) * S, (UINT)(k->y + 32) * S, 1};
    if (!r->texture || !native_scratch(n, box.right, box.bottom)) return;
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)d3d->scratch, 0, box.left, box.top, 0,
                                             (ID3D11Resource *)r->texture, 0, &box);
    D3D11_BOX page = {0, 0, 0, 64, 32, 1};
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)d3d->import, 0, &page, k->pixels, 64 * 4, 0);
    float map[4] = {(float)k->x, (float)k->y, (float)S, 0};
    D3D11_VIEWPORT viewport = {(float)box.left, (float)box.top,
                              (float)(64 * S), (float)(32 * S), 0, 1};
    D3D11_RECT rect = {(LONG)box.left, (LONG)box.top, (LONG)box.right, (LONG)box.bottom};
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)d3d->blit_constants, 0, NULL, map, 0, 0);
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &r->rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(c, NULL, NULL, ~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(c, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_VSSetShader(c, d3d->present_vertex, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, d3d->alpha_upload_pixel, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 5, 1, &d3d->blit_constants);
    ID3D11DeviceContext_PSSetShaderResources(c, 5, 1, &d3d->scratch_view);
    ID3D11DeviceContext_PSSetShaderResources(c, 7, 1, &d3d->import_view);
    ID3D11DeviceContext_RSSetViewports(c, 1, &viewport);
    ID3D11DeviceContext_RSSetScissorRects(c, 1, &rect);
    ID3D11DeviceContext_Draw(c, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(c, 5, 1, &none);
    ID3D11DeviceContext_PSSetShaderResources(c, 7, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    native_bind_pipeline(n);
}

/* Rebuilds conversion i of the frame into its texture; returns the view to sample, or NULL. */
static ID3D11ShaderResourceView *native_run_convert(Native *n, const NativeFrame *f, unsigned i)
{
    NativeD3d *d3d = n->gpu;
    ID3D11DeviceContext *c = d3d->context;
    const NativeConvert *v = &f->converts[i];
    unsigned w = v->constants.size[0], h = v->constants.size[1];
    if (!d3d->convert_shader) return NULL;
    ID3D11ShaderResourceView *views[9] = {v->base ? (ID3D11ShaderResourceView *)v->base->view : NULL};
    NativeConvertConstants *constants = &d3d->convert_scratch;
    *constants = v->constants;
    constants->tex[3] = 0;   /* each source's scale, a byte per slot */
    for (unsigned k = 0; k < 4; k++) {
        const NativeSurface *s = v->surface[k] >= 0 ? &n->surfaces[v->surface[k]] : NULL;
        if (s && !d3d_surface(n, s)->srv) return NULL;
        views[1 + k + (s && s->depth ? 4 : 0)] = s ? d3d_surface(n, s)->srv : NULL;
        constants->tex[3] |= (s ? s->scale : 1) << (k * 8);
    }
    if (d3d->converted[i].w != w || d3d->converted[i].h != h) {
        if (d3d->converted[i].uav) ID3D11UnorderedAccessView_Release(d3d->converted[i].uav);
        if (d3d->converted[i].srv) ID3D11ShaderResourceView_Release(d3d->converted[i].srv);
        if (d3d->converted[i].texture) ID3D11Texture2D_Release(d3d->converted[i].texture);
        memset(&d3d->converted[i], 0, sizeof d3d->converted[i]);
        D3D11_TEXTURE2D_DESC t = {0};
        t.Width = w; t.Height = h; t.MipLevels = t.ArraySize = 1; t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(ID3D11Device_CreateTexture2D(d3d->device, &t, NULL, &d3d->converted[i].texture)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(d3d->device, (ID3D11Resource *)d3d->converted[i].texture, NULL, &d3d->converted[i].srv)) ||
            FAILED(ID3D11Device_CreateUnorderedAccessView(d3d->device, (ID3D11Resource *)d3d->converted[i].texture, NULL, &d3d->converted[i].uav)))
            return NULL;
        d3d->converted[i].w = w; d3d->converted[i].h = h;
    }
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)d3d->convert_constants, 0, NULL, constants, 0, 0);
    ID3D11DeviceContext_CSSetShader(c, d3d->convert_shader, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 4, 1, &d3d->convert_constants);
    ID3D11DeviceContext_CSSetShaderResources(c, 6, 1, &d3d->inverse_view);
    ID3D11DeviceContext_CSSetShaderResources(c, 8, 9, views);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 2, 1, &d3d->converted[i].uav, NULL);
    ID3D11DeviceContext_Dispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
    ID3D11ShaderResourceView *none[9] = {NULL};
    ID3D11UnorderedAccessView *no_uav = NULL;
    ID3D11DeviceContext_CSSetShaderResources(c, 8, 9, none);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 2, 1, &no_uav, NULL);
    n->stats.conversions_run++;
    return d3d->converted[i].srv;
}

/* The field's commands, with these vertices, into the surfaces (native.h). */
void native_gpu_draw(Native *n, NativeFrame *f, const NativeVertex *vertices)
{
    NativeD3d *d3d = n->gpu;
    ID3D11DeviceContext *c = d3d->context;
    if (f->vertex_count > d3d->vertex_buffer_capacity) {
        if (d3d->vertex_buffer) ID3D11Buffer_Release(d3d->vertex_buffer);
        d3d->vertex_buffer = NULL;
        unsigned capacity = d3d->vertex_buffer_capacity ? d3d->vertex_buffer_capacity : 1 << 18;
        while (capacity < f->vertex_count) capacity *= 2;
        D3D11_BUFFER_DESC b = {0};
        b.ByteWidth = capacity * sizeof(NativeVertex); b.Usage = D3D11_USAGE_DYNAMIC;
        b.BindFlags = D3D11_BIND_VERTEX_BUFFER; b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(d3d->device, &b, NULL, &d3d->vertex_buffer))) { d3d->vertex_buffer_capacity = 0; return; }
        d3d->vertex_buffer_capacity = capacity;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (f->vertex_count && SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)d3d->vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, vertices, f->vertex_count * sizeof(NativeVertex));
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)d3d->vertex_buffer, 0);
    }
    native_bind_pipeline(n);
    int bound_target = -2, bound_depth = -2, last_convert = -1;
    for (unsigned i = 0; i < f->command_count; i++) {
        NativeCommand *k = &f->commands[i];
        if (k->kind == NATIVE_ALPHA_UPLOAD) {
            native_alpha_upload(n, k);
            bound_target = bound_depth = -2; last_convert = -1;
            continue;
        }
        if (k->kind == NATIVE_IMPORT) {
            NativeSurface *s = &n->surfaces[k->target];
            ID3D11Texture2D *texture = d3d->surfaces[k->target].texture;
            D3D11_BOX box = {(UINT)k->x, (UINT)k->y, 0, (UINT)(k->x + k->w), (UINT)(k->y + k->h), 1};
            last_convert = -1;
            if (!texture) continue;
            if (s->scale == 1)
                ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)texture, 0, &box, k->pixels, (UINT)k->w * 4, 0);
            else if (k->w <= 64 && k->h <= 64) {
                /* Upscaled: the page goes to a 1x texture and is drawn in, a block per pixel. */
                D3D11_BOX page = {0, 0, 0, (UINT)k->w, (UINT)k->h, 1};
                ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)d3d->import, 0, &page, k->pixels, (UINT)k->w * 4, 0);
                native_blit(n, d3d->import_view, 1, 0, 0, s, k->x, k->y, k->w, k->h);
                bound_target = bound_depth = -2;
            }
            d3d->drawn.imports++; d3d->drawn.import_px += (uint64_t)k->w * k->h;
            continue;
        }
        if (k->kind == NATIVE_COPY) {
            NativeSurface *s = &n->surfaces[k->source], *t = &n->surfaces[k->target];
            last_convert = -1;
            if (!d3d->surfaces[k->source].texture || !d3d->surfaces[k->target].texture) continue;
            if (s->scale != t->scale) bound_target = bound_depth = -2;
            native_copy(n, s, t, k->sx, k->sy, k->x, k->y, k->w, k->h);
            continue;
        }
        if (!k->count) continue;
        NativeSurface *target = &n->surfaces[k->target];
        NativeSurface *depth = k->depth >= 0 && (k->depth_key & 8) ? &n->surfaces[k->depth] : NULL;
        ID3D11RenderTargetView *rtv = d3d->surfaces[k->target].rtv;
        ID3D11DepthStencilView *dsv = depth ? d3d->surfaces[k->depth].dsv : NULL;
        if (!rtv) continue;
        if (depth && (!dsv || depth->size[0] != target->size[0] || depth->size[1] != target->size[1])) depth = NULL;
        unsigned T = target->scale, S = 1;
        ID3D11ShaderResourceView *view = NULL;
        if (k->texture_surface >= 0) {
            NativeSurface *s = &n->surfaces[k->texture_surface];
            view = d3d->surfaces[k->texture_surface].srv; S = s->scale;
            if (k->texture_surface == k->target) {
                /* Sampling the target: draw from a copy of the region the texture can reach. */
                int x = k->constants.tex_offset[0], y = k->constants.tex_offset[1];
                int w = (int)k->constants.tex_size[0], h = (int)k->constants.tex_size[1];
                if (x + w > (int)(s->size[0] / S)) w = (int)(s->size[0] / S) - x;
                if (y + h > (int)(s->size[1] / S)) h = (int)(s->size[1] / S) - y;
                if (w < 0) w = 0;
                if (h < 0) h = 0;
                if (!native_scratch(n, (unsigned)(x + w) * S, (unsigned)(y + h) * S)) continue;
                D3D11_BOX box = {(UINT)x * S, (UINT)y * S, 0, (UINT)(x + w) * S, (UINT)(y + h) * S, 1};
                ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
                bound_target = bound_depth = -2;
                if (w && h)
                    ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)d3d->scratch, 0, box.left, box.top, 0,
                                                              (ID3D11Resource *)d3d->surfaces[k->texture_surface].texture, 0, &box);
                d3d->drawn.self_copies++; d3d->drawn.self_copy_px += (uint64_t)w * h;
                view = d3d->scratch_view;
            }
        } else if (k->replacement) {
            view = (ID3D11ShaderResourceView *)k->replacement;
            const uint32_t *wrap = k->constants.wrap;
            unsigned which = (k->constants.flags >> 5 & 1) | (wrap[0] != 0) << 1 | (wrap[1] != 0) << 2;
            ID3D11DeviceContext_PSSetSamplers(c, 1, 1, &d3d->replace_samplers[which]);
        } else if (k->texture) view = (ID3D11ShaderResourceView *)k->texture->view;
        else if (k->convert >= 0) {
            /* Again for each run of draws: the surface may have changed since. */
            static ID3D11ShaderResourceView *converted;
            if (k->convert != last_convert) {
                ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
                bound_target = bound_depth = -2;
                converted = native_run_convert(n, f, (unsigned)k->convert);
                last_convert = k->convert;
            }
            view = converted;
            if (!view) continue;
        }
        if (k->convert < 0) last_convert = -1;
        if (k->target != bound_target || (depth ? k->depth : -1) != bound_depth) {
            ID3D11ShaderResourceView *none = NULL;
            ID3D11DeviceContext_PSSetShaderResources(c, 4, 1, &none);
            ID3D11DeviceContext_OMSetRenderTargets(c, 1, &rtv, depth ? dsv : NULL);
            /* GS coordinates stay; the viewport scales them (a vertex at x lands at (x + 0.5) * T). */
            D3D11_VIEWPORT viewport = {0, 0, 2048.0f * T, 2048.0f * T, 0, 1};
            ID3D11DeviceContext_RSSetViewports(c, 1, &viewport);
            bound_target = k->target; bound_depth = depth ? k->depth : -1;
        }
        ID3D11DeviceContext_PSSetShaderResources(c, 4, 1, &view);
        float fix = k->blend_fix / 128.0f;
        float factor[4] = {fix, fix, fix, fix};
        ID3D11DeviceContext_OMSetBlendState(c, native_blend_state(n, k->blend_key, k->color_mask), factor, ~0u);
        ID3D11DeviceContext_OMSetDepthStencilState(c, native_depth_state(n, depth ? k->depth_key : 0), 0);
        D3D11_RECT scissor = {k->scissor.left * (LONG)T, k->scissor.top * (LONG)T, k->scissor.right * (LONG)T,
                              k->scissor.bottom * (LONG)T};
        ID3D11DeviceContext_RSSetScissorRects(c, 1, &scissor);
        if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)d3d->constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            NativeConstants *constants = mapped.pData;
            *constants = k->constants;
            constants->scale = T | S << 8 | (S < T ? S : T) << 16;
            ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)d3d->constant_buffer, 0);
        }
        ID3D11DeviceContext_Draw(c, k->count, k->first);
        d3d->drawn.draws++;
    }
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(c, 4, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
}

/* The displayed picture's texture (and its read-back copy) at w x h. */
static int native_display_size(Native *n, unsigned w, unsigned h)
{
    NativeD3d *d3d = n->gpu;
    if (n->display_width != w || n->display_height != h) {
        if (d3d->display_uav) ID3D11UnorderedAccessView_Release(d3d->display_uav);
        if (d3d->display_view) ID3D11ShaderResourceView_Release(d3d->display_view);
        d3d->display_view = NULL;
        if (d3d->display) ID3D11Texture2D_Release(d3d->display);
        if (d3d->display_readback) ID3D11Texture2D_Release(d3d->display_readback);
        d3d->display_uav = NULL; d3d->display = d3d->display_readback = NULL;
        n->display_width = n->display_height = 0;
        D3D11_TEXTURE2D_DESC t = {0};
        t.Width = w; t.Height = h; t.MipLevels = t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM; t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(d3d->device, &t, NULL, &d3d->display))) return 0;
        if (FAILED(ID3D11Device_CreateUnorderedAccessView(d3d->device, (ID3D11Resource *)d3d->display, NULL, &d3d->display_uav))) return 0;
        if (FAILED(ID3D11Device_CreateShaderResourceView(d3d->device, (ID3D11Resource *)d3d->display, NULL, &d3d->display_view))) return 0;
        t.Usage = D3D11_USAGE_STAGING; t.BindFlags = 0; t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(d3d->device, &t, NULL, &d3d->display_readback))) return 0;
        n->display_width = w; n->display_height = h;
    }
    return 1;
}

/* A movie's picture (gs_native_movie) as the displayed picture. */
int native_gpu_movie(Native *n, NativeFrame *f)
{
    NativeD3d *d3d = n->gpu;
    if (!native_display_size(n, f->movie_width, f->movie_height)) return 0;
    ID3D11DeviceContext_UpdateSubresource(d3d->context, (ID3D11Resource *)d3d->display, 0, NULL, f->movie,
                                          f->movie_width * 4, 0);
    return 1;
}

/* The display circuit's picture from the displayed surface (raster.hlsl native_scanout). */
int native_gpu_scanout(Native *n, NativeFrame *f)
{
    NativeD3d *d3d = n->gpu;
    ID3D11DeviceContext *c = d3d->context;
    if (f->display_surface < 0 || !d3d->surfaces[f->display_surface].srv) return 0;
    /* An upscaled display surface gives a picture as many times larger (raster.hlsl). */
    unsigned scale = n->surfaces[f->display_surface].scale;
    uint32_t d[12];
    memcpy(d, f->display, sizeof d);
    d[5] *= scale; d[6] *= scale; d[8] = scale;
    if (!native_display_size(n, d[5], d[6])) return 0;
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)d3d->display_constants, 0, NULL, d, 0, 0);
    ID3D11DeviceContext_CSSetShader(c, d3d->scanout_shader, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 1, 1, &d3d->display_constants);
    ID3D11DeviceContext_CSSetShaderResources(c, 5, 1, &d3d->surfaces[f->display_surface].srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 1, 1, &d3d->display_uav, NULL);
    ID3D11DeviceContext_Dispatch(c, (d[5] + 7) / 8, (d[6] + 7) / 8, 1);
    ID3D11ShaderResourceView *no_view = NULL;
    ID3D11UnorderedAccessView *no_uav = NULL;
    ID3D11DeviceContext_CSSetShaderResources(c, 5, 1, &no_view);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 1, 1, &no_uav, NULL);
    return 1;
}

/* ---- textures (GS worker) ---- */

int native_gpu_texture(Native *n, NativeTexture *t, const uint32_t *pixels, unsigned w, unsigned h, int sharp,
                       uint64_t key)
{
    NativeD3d *d3d = n->gpu;
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *view = NULL, *sharp_view = NULL;
    D3D11_TEXTURE2D_DESC d = {0};
    d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {pixels, w * 4, 0};
    if (FAILED(ID3D11Device_CreateTexture2D(d3d->device, &d, &data, &texture))) return 0;
    /* The text engine's font, made sharp for high resolutions (dhd.ini font, finding 058). */
    if (sharp) sharp_view = font_sharp_view(d3d->device, pixels, w, h, key);
    if (FAILED(ID3D11Device_CreateShaderResourceView(d3d->device, (ID3D11Resource *)texture, NULL, &view))) {
        if (sharp_view) ID3D11ShaderResourceView_Release(sharp_view);
        ID3D11Texture2D_Release(texture);
        return 0;
    }
    t->texture = (NativeImage *)texture; t->view = (NativeView *)view; t->sharp = (NativeView *)sharp_view;
    return 1;
}

void native_gpu_texture_free(Native *n, NativeTexture *t)
{
    (void)n;
    if (t->texture) {
        ID3D11ShaderResourceView_Release((ID3D11ShaderResourceView *)t->view);
        ID3D11Texture2D_Release((ID3D11Texture2D *)t->texture);
        t->texture = NULL; t->view = NULL;
    }
    if (t->sharp) { ID3D11ShaderResourceView_Release((ID3D11ShaderResourceView *)t->sharp); t->sharp = NULL; }
}

NativeView *native_gpu_pack_view(Native *n, TexPackEntry *e)
{
    return (NativeView *)texpack_view(e, n->field);
}

/* ---- presentation (render thread, context lock held) ---- */

/* The window's height, for DHD_SCALE=window. */
unsigned native_gpu_window_height(Native *n)
{
    DXGI_SWAP_CHAIN_DESC desc;
    RECT client = {0, 0, 0, 0};
    IDXGISwapChain *swapchain = n->target;
    if (swapchain && SUCCEEDED(IDXGISwapChain_GetDesc(swapchain, &desc))) GetClientRect(desc.OutputWindow, &client);
    return client.bottom > 0 ? (unsigned)client.bottom : 0;
}

void native_gpu_wait(Native *n)
{
    (void)n;
    gs_frame_latency_wait();
}

int native_gpu_present(Native *n)
{
    static const GUID texture_iid = {0x6F15AAF2,0xD208,0x4E89,{0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C}};
    NativeD3d *d3d = n->gpu;
    IDXGISwapChain *swapchain = n->target;
    if (!d3d->display || !swapchain) return 0;
    if (!d3d->window) {
        DXGI_SWAP_CHAIN_DESC desc;
        if (SUCCEEDED(IDXGISwapChain_GetDesc(swapchain, &desc))) d3d->window = desc.OutputWindow;
    }
    /* The back buffer is the window's size; the picture is scaled into it. */
    RECT client = {0, 0, 0, 0};
    if (d3d->window) GetClientRect(d3d->window, &client);
    unsigned w = (unsigned)client.right, h = (unsigned)client.bottom;
    if (!w || !h) return 0;  /* minimised */
    if (!d3d->backbuffer || d3d->buffer_width != w || d3d->buffer_height != h || n->resize) {
        if (d3d->back_view) { ID3D11RenderTargetView_Release(d3d->back_view); d3d->back_view = NULL; }
        if (d3d->backbuffer) { ID3D11Texture2D_Release(d3d->backbuffer); d3d->backbuffer = NULL; }
        HRESULT hr = IDXGISwapChain_ResizeBuffers(swapchain, 2, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, gs_swap_chain_flags());
        if (SUCCEEDED(hr)) hr = IDXGISwapChain_GetBuffer(swapchain, 0, &texture_iid, (void **)&d3d->backbuffer);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(d3d->device, (ID3D11Resource *)d3d->backbuffer, NULL, &d3d->back_view);
        if (FAILED(hr)) { fprintf(stderr, "gs native: resize failed (%08lX)\n", hr); return 0; }
        d3d->buffer_width = w; d3d->buffer_height = h; n->resize = 0;
    }
    ID3D11DeviceContext *c = d3d->context;
    float vw, vh;
    native_viewport(n, w, h, &vw, &vh);
    D3D11_VIEWPORT viewport = {floorf((w - vw) / 2), floorf((h - vh) / 2), vw, vh, 0, 1};
    /* DHD_VIEW (present.h gs_native_view): one quarter of the 3-4 player split screen (presets
       6-9) fills the picture: the whole is drawn twice as large, shifted and cut to the picture;
       every view is still drawn and the game is unchanged (finding 041). */
    int view = gs_native_view - 1;
    D3D11_RECT cut = {(LONG)viewport.TopLeftX, (LONG)viewport.TopLeftY, (LONG)(viewport.TopLeftX + vw),
                      (LONG)(viewport.TopLeftY + vh)};
    if (view >= 0 && view < 4) {
        viewport.TopLeftX -= (view & 1) * vw, viewport.TopLeftY -= (view >> 1) * vh;
        viewport.Width *= 2, viewport.Height *= 2;
    }
    static const float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(c, d3d->back_view, black);
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &d3d->back_view, NULL);
    ID3D11DeviceContext_OMSetBlendState(c, NULL, NULL, ~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(c, NULL, 0);
    ID3D11DeviceContext_RSSetState(c, d3d->rasterizer);  /* scissor on */
    ID3D11DeviceContext_RSSetScissorRects(c, 1, &cut);
    ID3D11DeviceContext_RSSetViewports(c, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, d3d->present_vertex, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, d3d->present_pixel, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &d3d->display_view);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &d3d->present_sampler);
    ID3D11DeviceContext_Draw(c, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    if (n->interp) IDXGISwapChain_Present(swapchain, !n->interp_hz ? 1 : 0, 0);
    else {
        pacing_present(n->present_due, n->present_began);  /* holds the picture to its due time */
        gs_present(swapchain);
    }
    return 1;
}

/* The displayed picture into pixels (display_width x display_height). */
int native_gpu_read(Native *n, uint32_t *pixels)
{
    NativeD3d *d3d = n->gpu;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!d3d->display_readback) return 0;
    ID3D11DeviceContext_CopyResource(d3d->context, (ID3D11Resource *)d3d->display_readback, (ID3D11Resource *)d3d->display);
    if (FAILED(ID3D11DeviceContext_Map(d3d->context, (ID3D11Resource *)d3d->display_readback, 0, D3D11_MAP_READ, 0, &mapped)))
        return 0;
    for (unsigned y = 0; y < n->display_height; y++)
        memcpy(pixels + y * n->display_width, (const uint8_t *)mapped.pData + y * mapped.RowPitch, n->display_width * 4);
    ID3D11DeviceContext_Unmap(d3d->context, (ID3D11Resource *)d3d->display_readback, 0);
    return 1;
}

/* DHD_NATIVE_TRACE: writes the pictures whose GPU timestamps have arrived, oldest first (all of
   them when asked to wait). Context lock held. */
static void native_gpu_collect(Native *n, int wait)
{
    NativeD3d *d3d = n->gpu;
    for (unsigned k = 0; k < 8; k++) {
        unsigned i = (d3d->gpu_time_next + k) % 8;
        if (!d3d->gpu_time[i].pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d;
        UINT64 a = 0, b = 0;
        UINT flags = wait ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
        HRESULT hr;
        while ((hr = ID3D11DeviceContext_GetData(d3d->context, (ID3D11Asynchronous *)d3d->gpu_time[i].disjoint, &d, sizeof d, flags)) == S_FALSE && wait)
            YieldProcessor();
        if (hr != S_OK) return;  /* in order: later ones are not ready either */
        double gpu = -1;
        if (!d.Disjoint && d.Frequency &&
            ID3D11DeviceContext_GetData(d3d->context, (ID3D11Asynchronous *)d3d->gpu_time[i].begin, &a, sizeof a, 0) == S_OK &&
            ID3D11DeviceContext_GetData(d3d->context, (ID3D11Asynchronous *)d3d->gpu_time[i].end, &b, sizeof b, 0) == S_OK)
            gpu = (double)(b - a) * 1000.0 / (double)d.Frequency;
        fprintf(n->trace_pictures, "%llu,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                (unsigned long long)d3d->gpu_time[i].field, d3d->gpu_time[i].t, d3d->gpu_time[i].cpu_ms, gpu,
                d3d->gpu_time[i].draws, d3d->gpu_time[i].self_copies, d3d->gpu_time[i].self_copy_px, d3d->gpu_time[i].copies,
                d3d->gpu_time[i].copy_px, d3d->gpu_time[i].imports, d3d->gpu_time[i].import_px);
        d3d->gpu_time[i].pending = 0;
    }
}

/* DHD_NATIVE_TRACE: a picture's GPU time, between timestamps at its start and before Present (so
   the vsync wait isn't counted as GPU work), with what it drew. */
void native_gpu_time(Native *n, int stage, const NativeFrame *f, float t, float cpu_ms)
{
    NativeD3d *d3d = n->gpu;
    unsigned slot = d3d->gpu_time_next % 8;
    if (stage == NATIVE_TIME_BEGIN) {
        if (d3d->gpu_time[slot].pending) native_gpu_collect(n, 1);
        if (!d3d->gpu_time[slot].disjoint) {
            D3D11_QUERY_DESC q = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            ID3D11Device_CreateQuery(d3d->device, &q, &d3d->gpu_time[slot].disjoint);
            q.Query = D3D11_QUERY_TIMESTAMP;
            ID3D11Device_CreateQuery(d3d->device, &q, &d3d->gpu_time[slot].begin);
            ID3D11Device_CreateQuery(d3d->device, &q, &d3d->gpu_time[slot].end);
        }
        memset(&d3d->drawn, 0, sizeof d3d->drawn);
        ID3D11DeviceContext_Begin(d3d->context, (ID3D11Asynchronous *)d3d->gpu_time[slot].disjoint);
        ID3D11DeviceContext_End(d3d->context, (ID3D11Asynchronous *)d3d->gpu_time[slot].begin);
    } else if (stage == NATIVE_TIME_DRAWN)
        ID3D11DeviceContext_End(d3d->context, (ID3D11Asynchronous *)d3d->gpu_time[slot].end);
    else {
        ID3D11DeviceContext_End(d3d->context, (ID3D11Asynchronous *)d3d->gpu_time[slot].disjoint);
        d3d->gpu_time[slot].pending = 1;
        d3d->gpu_time[slot].field = f->field; d3d->gpu_time[slot].t = t;
        d3d->gpu_time[slot].cpu_ms = cpu_ms;
        d3d->gpu_time[slot].draws = d3d->drawn.draws;
        d3d->gpu_time[slot].self_copies = d3d->drawn.self_copies; d3d->gpu_time[slot].self_copy_px = d3d->drawn.self_copy_px;
        d3d->gpu_time[slot].copies = d3d->drawn.copies; d3d->gpu_time[slot].copy_px = d3d->drawn.copy_px;
        d3d->gpu_time[slot].imports = d3d->drawn.imports; d3d->gpu_time[slot].import_px = d3d->drawn.import_px;
        d3d->gpu_time_next++;
        native_gpu_collect(n, 0);
    }
}

/* ---- set-up ---- */

static ID3DBlob *native_compile(const char *entry, const char *target)
{
    static const D3D_SHADER_MACRO defines[] = {{"DHD_NATIVE", "1"}, {NULL, NULL}};
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = shader_compile(defines, entry, target, D3DCOMPILE_ENABLE_STRICTNESS, &code, &errors);
    if (errors) { fprintf(stderr, "%s", (const char *)ID3D10Blob_GetBufferPointer(errors)); ID3D10Blob_Release(errors); }
    return SUCCEEDED(hr) ? code : NULL;
}

/* DHD_PRESENT_FILTER=point keeps the picture's pixels sharp; linear by default. */
int native_gpu_present_filter(Native *n, int point)
{
    NativeD3d *d3d = n->gpu;
    D3D11_SAMPLER_DESC sampler = {point ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                                  D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
    ID3D11SamplerState *state = NULL;
    if (FAILED(ID3D11Device_CreateSamplerState(d3d->device, &sampler, &state))) return 0;
    if (d3d->present_sampler) ID3D11SamplerState_Release(d3d->present_sampler);
    d3d->present_sampler = state;
    return 1;
}

/* The native renderer on this device (the presentation's); the exact backend, on a device of its
   own, answers the Z probe (host). */
static int native_init(Gs *g, NativeHost *host, ID3D11Device *device, ID3D11DeviceContext *context)
{
    NativeD3d *d3d = calloc(1, sizeof *d3d);
    Native *n = d3d ? native_create() : NULL;
    if (!n) { free(d3d); return 0; }
    n->gpu = d3d; d3d->device = device; d3d->context = context;
    ID3DBlob *vs = native_compile("native_vertex", "vs_5_0");
    ID3DBlob *ps = native_compile("native_pixel", "ps_5_0");
    ID3DBlob *cs = native_compile("native_scanout", "cs_5_0");
    ID3DBlob *convert = native_compile("native_convert", "cs_5_0");
    ID3DBlob *present_vs = native_compile("present_vertex", "vs_5_0"), *present_ps = native_compile("present_pixel", "ps_5_0");
    HRESULT hr = vs && ps && cs && present_vs && present_ps ? S_OK : E_FAIL;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(present_vs), ID3D10Blob_GetBufferSize(present_vs), NULL, &d3d->present_vertex);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(present_ps), ID3D10Blob_GetBufferSize(present_ps), NULL, &d3d->present_pixel);
    if (present_vs) ID3D10Blob_Release(present_vs);
    if (present_ps) ID3D10Blob_Release(present_ps);
    if (SUCCEEDED(hr) && !native_present_options(n)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL, &d3d->vertex_shader);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &d3d->pixel_shader);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateComputeShader(device, ID3D10Blob_GetBufferPointer(cs), ID3D10Blob_GetBufferSize(cs), NULL, &d3d->scanout_shader);
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateInputLayout(device, layout, 3, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), &d3d->layout);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    if (cs) ID3D10Blob_Release(cs);
    if (SUCCEEDED(hr) && convert)
        hr = ID3D11Device_CreateComputeShader(device, ID3D10Blob_GetBufferPointer(convert), ID3D10Blob_GetBufferSize(convert), NULL, &d3d->convert_shader);
    if (convert) ID3D10Blob_Release(convert);
    D3D11_BUFFER_DESC b = {0};
    b.ByteWidth = sizeof(NativeConstants); b.Usage = D3D11_USAGE_DYNAMIC;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER; b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(device, &b, NULL, &d3d->constant_buffer);
    b.ByteWidth = 48; b.Usage = D3D11_USAGE_DEFAULT; b.CPUAccessFlags = 0;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(device, &b, NULL, &d3d->display_constants);
    b.ByteWidth = sizeof(NativeConvertConstants);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(device, &b, NULL, &d3d->convert_constants);
    {
        static uint32_t inverse[4096];
        native_page_inverse(inverse);
        D3D11_BUFFER_DESC ib = {sizeof inverse, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0,
                                D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
        D3D11_SUBRESOURCE_DATA id = {inverse, 0, 0};
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(device, &ib, &id, &d3d->inverse);
        D3D11_SHADER_RESOURCE_VIEW_DESC iv = {DXGI_FORMAT_UNKNOWN, D3D11_SRV_DIMENSION_BUFFER};
        iv.Buffer.NumElements = 4096;
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)d3d->inverse, &iv, &d3d->inverse_view);
    }
    D3D11_RASTERIZER_DESC r = {0};
    r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE; r.ScissorEnable = TRUE; r.DepthClipEnable = FALSE;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRasterizerState(device, &r, &d3d->rasterizer);
    ID3DBlob *blit = native_compile("native_blit", "ps_5_0");
    if (SUCCEEDED(hr)) hr = blit ? ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(blit), ID3D10Blob_GetBufferSize(blit), NULL, &d3d->blit_pixel) : E_FAIL;
    if (blit) ID3D10Blob_Release(blit);
    ID3DBlob *alpha_upload = native_compile("native_upload_alpha", "ps_5_0");
    if (SUCCEEDED(hr)) hr = alpha_upload ? ID3D11Device_CreatePixelShader(device,
        ID3D10Blob_GetBufferPointer(alpha_upload), ID3D10Blob_GetBufferSize(alpha_upload), NULL, &d3d->alpha_upload_pixel) : E_FAIL;
    if (alpha_upload) ID3D10Blob_Release(alpha_upload);
    D3D11_BUFFER_DESC bb = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(device, &bb, NULL, &d3d->blit_constants);
    D3D11_TEXTURE2D_DESC t = {0};
    t.Width = t.Height = 64; t.MipLevels = t.ArraySize = 1; t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateTexture2D(device, &t, NULL, &d3d->import);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)d3d->import, NULL, &d3d->import_view);
    if (FAILED(hr)) { fprintf(stderr, "gs native: initialization failed (%08lX)\n", hr); return 0; }
    /* A texture pack (dhd.ini texture_pack, textures): replacements are sampled with mipmaps,
       anisotropic when the GS filters (trilinear alone blurs ground seen at a low angle), point
       magnification when it doesn't, with clamped or repeating coordinates as the GS has them. */
    int pack = texpack_open(device);
    if (pack || font_sharp_on()) {
        for (unsigned i = 0; i < 8 && SUCCEEDED(hr); i++) {
            D3D11_SAMPLER_DESC sd = {0};
            sd.Filter = i & 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
            sd.MaxAnisotropy = i & 1 ? 16 : 1;
            sd.AddressU = i & 2 ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
            sd.AddressV = i & 4 ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
            sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            hr = ID3D11Device_CreateSamplerState(device, &sd, &d3d->replace_samplers[i]);
        }
        n->texpack = pack && SUCCEEDED(hr);
        n->replacements = SUCCEEDED(hr);
        if (FAILED(hr)) memset(d3d->replace_samplers, 0, sizeof d3d->replace_samplers);
        hr = S_OK;
    }
    ID3D11Device_AddRef(device); ID3D11DeviceContext_AddRef(context);
    native_start(g, n, host);
    return 1;
}

/* The field to the render thread, to be presented in this swap chain (native_vsync). */
void gs_native_vsync(Gs *g, IDXGISwapChain *swapchain, unsigned *width, unsigned *height, int resize)
{
    native_vsync(g, swapchain, width, height, resize);
}
