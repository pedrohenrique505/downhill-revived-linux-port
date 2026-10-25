/* Native transfer ordering, using synthetic solid colours and no game data.
   The presenter, texture replacements and EE scheduler are absent. */
#define COBJMACROS
#ifdef DHD_NATIVE_VULKAN
#include "../platform/linux/gpu.h"
#include "../platform/linux/native_gpu.h"
#define gs_native_vsync native_vsync
uint32_t *gs_native_picture(Gs *, unsigned *, unsigned *);
#else
#include "gpu_win32.h"
#endif
#include "present.h"
#include "gpu.h"
#include "texpack.h"
#include "font_sharp.h"
#include <windows.h>
uint64_t kernel_vsync_count;
volatile int gs_native_view;
double host_seconds(void) { return GetTickCount64()/1000.0; }
double host_wall_seconds(void) { return host_seconds(); }
void host_thread_name(const char *n) {(void)n;}
void pacing_handed(double *d,double *b) {*d=*b=0;}
void pacing_present(double d,double b) {(void)d;(void)b;}
int host_widescreen_flat_limit(void) {return 1000;}
double host_widescreen_aspect(void) {return 16.0/9;}
#ifndef DHD_NATIVE_VULKAN
UINT gs_swap_chain_flags(void) {return 0;}
void gs_frame_latency_wait(void) {}
HRESULT gs_present(IDXGISwapChain *s) {return IDXGISwapChain_Present(s,0,0);}
int texpack_open(ID3D11Device *d) {return 0;}
int texpack_available(void) {return 0;}
TexPackEntry *texpack_find(const Gs *g,unsigned c) {return NULL;}
ID3D11ShaderResourceView *texpack_view(TexPackEntry *e,uint64_t f) {return NULL;}
void texpack_field(uint64_t f) {}
void texpack_report(void) {}
int font_sharp_on(void) {return 0;}
int font_sharp_match(uint64_t t) {return 0;}
ID3D11ShaderResourceView *font_sharp_view(ID3D11Device *d,const uint32_t *p,unsigned w,unsigned h,uint64_t k) {return NULL;}
#else
int texpack_available(void) { return 0; }
TexPackEntry *texpack_find(const Gs *g, unsigned c) { (void)g; (void)c; return NULL; }
void texpack_field(uint64_t f) { (void)f; }
void texpack_report(void) {}
int font_sharp_on(void) { return 0; }
int font_sharp_match(uint64_t t) { (void)t; return 0; }
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { WIDTH = 512, HEIGHT = 224 };
static unsigned failures;

static void clear(unsigned page, uint32_t color)
{
    gs_write(&gs, GS_FRAME_1, page | 8ULL << 16);
    gs_write(&gs, GS_ZBUF_1, 1ULL << 32);
    gs_write(&gs, GS_TEST_1, 0);
    gs_write(&gs, GS_SCISSOR_1, (WIDTH - 1ULL) << 16 | (HEIGHT - 1ULL) << 48);
    gs_write(&gs, GS_PRIM, 6);
    gs_write(&gs, GS_RGBAQ, color);
    gs_write(&gs, GS_XYZ2, 0);
    gs_write(&gs, GS_XYZ2, WIDTH * 16ULL | (HEIGHT * 16ULL) << 16);
}

static void show(unsigned page, uint32_t color, const char *name)
{
    gs_priv_write(&gs, GS_PMODE, 2);
    gs_priv_write(&gs, GS_SMODE2, 0);
    gs_priv_write(&gs, GS_DISPFB2, page | 8ULL << 9);
    gs_priv_write(&gs, GS_DISPLAY2, (WIDTH - 1ULL) << 32 | (HEIGHT - 1ULL) << 44);
    unsigned w = 0, h = 0;
    ++kernel_vsync_count;
    gs_native_vsync(&gs, NULL, &w, &h, 0);
    uint32_t *pixels = gs_native_picture(&gs, &w, &h);
    unsigned changed = 0;
    if (pixels)
        for (unsigned i = 0; i < w * h; i++)
            changed += (pixels[i] & 0xFFFFFF) != (color & 0xFFFFFF);
    int ok = pixels && w == WIDTH * 2 && h == HEIGHT * 2 && !changed;
    if (!ok) failures++;
    printf("%s: %s (%ux%u, %u incorrect pixels)\n", name, ok ? "PASS" : "FAIL", w, h, changed);
}

static void sample_alpha(unsigned x, unsigned y, unsigned alpha, const char *name)
{
    clear(256, 0);
    /* CSM2 palette: index becomes the red channel, making stored alpha visible. */
    for (unsigned i = 0; i < 256; i++) gs_write_pixel(&gs, GS_PSMCT32, 14000, 4, i, 0, 0x80000000 | i);
    gs_write(&gs, GS_TEXCLUT, 4);
    gs_write(&gs, GS_TEX0_1, 4096ULL | 8ULL << 14 | (uint64_t)GS_PSMT8H << 20 |
             9ULL << 26 | 8ULL << 30 | 1ULL << 35 | 14000ULL << 37 | 1ULL << 55 | 1ULL << 61);
    gs_write(&gs, GS_PRIM, 6 | 16 | 256);
    gs_write(&gs, GS_RGBAQ, 0x3F80000080808080ULL);
    gs_write(&gs, GS_UV, (x * 16 + 8) | (uint64_t)(y * 16 + 8) << 16);
    gs_write(&gs, GS_XYZ2, 0);
    gs_write(&gs, GS_XYZ2, WIDTH * 16ULL | (HEIGHT * 16ULL) << 16);
    show(256, alpha, name);
}

#ifdef DHD_NATIVE_VULKAN
/* Backend seams the game's transfer stream does not always exercise: mixed scales,
   overlapping self copies, target feedback, surface growth and reversed depth. */
static void backend_cases(void)
{
    Native *n = native_of(&gs);
    EnterCriticalSection(&n->context_lock);
    int base = n->surface_count;
    n->surface_count += 3;
    NativeSurface *large = &n->surfaces[base], *small = large+1, *depth = large+2;
    large->scale = 2; small->scale = 1; depth->scale = 2; depth->depth = 1;
    unsigned want[2] = {64,64};
    native_gpu_surface(n, large, want); native_gpu_surface(n, small, want); native_gpu_surface(n, depth, want);
    uint32_t page[64*64];
    for (unsigned i = 0; i < 64*64; i++) page[i] = i%64 < 32 ? 0x800000ff : 0x8000ff00;
    NativeCommand commands[3] = {{.kind=NATIVE_IMPORT,.target=base+1,.w=64,.h=64,.pixels=page},
        {.kind=NATIVE_COPY,.source=base+1,.target=base,.w=64,.h=64},
        {.kind=NATIVE_COPY,.source=base,.target=base,.sx=0,.sy=0,.x=8,.y=0,.w=56,.h=64}};
    NativeFrame f = {.commands=commands,.command_count=3,.display_surface=base,
        .display={0,0,0,0,0,64,64,0,0,0,0,0}};
    native_gpu_draw(n, &f, NULL); native_gpu_scanout(n, &f);
    uint32_t pixels[128*128];
    int ok = native_gpu_read(n, pixels);
    for (unsigned y=0; y<128; y++) for (unsigned x=0; x<128; x++)
        if ((pixels[y*128+x]&0xffffff) != (x<80 ? 0xffu : 0xff00u)) ok=0;
    printf("mixed-scale blit and overlapping self copy: %s\n", ok ? "PASS":"FAIL"); failures += !ok;
    /* Growing keeps the old colour pixels; the new area is cleared. */
    want[0]=128;
    native_gpu_surface(n, large, want); native_gpu_surface(n, depth, want);
    f.command_count=0; f.display[5]=128;
    native_gpu_draw(n,&f,NULL); native_gpu_scanout(n,&f);
    uint32_t grown[256*128]; ok=native_gpu_read(n,grown);
    for (unsigned y=0; y<128; y++) for (unsigned x=0; x<256; x++)
        if ((grown[y*256+x]&0xffffff) != (x<80 ? 0xffu : x<128 ? 0xff00u : 0)) ok=0;
    printf("grown surface preserves old pixels and clears new extent: %s\n",ok?"PASS":"FAIL"); failures += !ok;
    /* Six vertices cover the GS surface. The second draw's greater test must fail. */
    NativeVertex vertices[12];
    static const float xy[6][2]={{0,0},{128,0},{0,64},{0,64},{128,0},{128,64}};
    for(unsigned i=0;i<12;i++) vertices[i]=(NativeVertex){.x=xy[i%6][0],.y=xy[i%6][1],
        .z=i<6 ? 0.75f:0.25f,.q=1,.rgba=i<6 ? 0x800000ff:0x8000ff00};
    NativeCommand draws[2]={{.kind=NATIVE_DRAW,.target=base,.depth=base+2,.texture_surface=-1,.convert=-1,
        .depth_key=8|4|1,.color_mask=15,.scissor={0,0,128,64},.constants={.target={2048,2048}},.count=6},
        {.kind=NATIVE_DRAW,.target=base,.depth=base+2,.texture_surface=-1,.convert=-1,
        .depth_key=8|4|3,.color_mask=15,.scissor={0,0,128,64},.constants={.target={2048,2048}},.first=6,.count=6}};
    f.commands=draws; f.command_count=2; f.vertices=vertices; f.vertex_count=12;
    native_gpu_draw(n,&f,vertices); native_gpu_scanout(n,&f); ok=native_gpu_read(n,grown);
    for(unsigned i=0;i<256*128;i++) if((grown[i]&0xffffff)!=0xff) ok=0;
    printf("reversed GS depth test: %s\n",ok?"PASS":"FAIL"); failures += !ok;
    /* Fill magenta first: sampling a dummy/black image would erase blue, and ignoring
       the colour mask would let the vertex colour erase red. */
    for (unsigned i = 0; i < 6; i++) vertices[i].rgba = 0x80ff00ff;
    draws[0].depth = -1; f.command_count = 1;
    native_gpu_draw(n, &f, vertices);
    /* A snapshot of the target is sampled while blue alone is written; red stays. */
    for(unsigned i=0;i<6;i++) {vertices[i].s=xy[i][0]/128;vertices[i].t=xy[i][1]/64; vertices[i].rgba=0x80808000;}
    draws[0].depth=-1; draws[0].texture_surface=base; draws[0].color_mask=4;
    draws[0].constants.flags=1; /* texture modulation; red becomes zero before write mask */
    draws[0].constants.tex_size[0]=128; draws[0].constants.tex_size[1]=64;
    draws[0].constants.alpha_scale=1; f.command_count=1;
    native_gpu_draw(n,&f,vertices); native_gpu_scanout(n,&f); ok=native_gpu_read(n,grown);
    for(unsigned i=0;i<256*128;i++) if((grown[i]&0xffffff)!=0xff00ff) ok=0;
    printf("target feedback snapshot with colour mask: %s\n",ok?"PASS":"FAIL"); failures += !ok;
    LeaveCriticalSection(&n->context_lock);
}
#endif

int main(void)
{
    gs_reset(&gs);
    _putenv("DHD_RENDER=native");
    _putenv("DHD_SCALE=2");
#ifdef DHD_NATIVE_VULKAN
    if (!gs_vulkan_init(&gs) || !gs_vulkan_native(&gs)) return 2;
#else
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                               D3D11_SDK_VERSION, &device, NULL, &context)) ||
        !gs_gpu_init(&gs, device, context) || !gs_gpu_native(&gs)) return 2;
#endif

    /* The upload is later than the clear, even though the native draw batch is open. */
    clear(0, 0x80000000);
    gs_write(&gs, GS_BITBLTBUF, 8ULL << 48);
    gs_write(&gs, GS_TRXPOS, 0);
    gs_write(&gs, GS_TRXREG, WIDTH | (uint64_t)HEIGHT << 32);
    gs_write(&gs, GS_TRXDIR, 0);
    for (unsigned i = 0; i < WIDTH * HEIGHT / 2; i++)
        gs_write(&gs, GS_HWREG, 0x800000FF800000FFULL);
    show(0, 0x800000FF, "clear then full-page CPU upload");

    /* A newly drawn source has no older surface owner for the copy to find. */
    clear(128, 0x8000FF00);
    gs_write(&gs, GS_BITBLTBUF, 4096ULL | 8ULL << 16 | 8192ULL << 32 | 8ULL << 48);
    gs_write(&gs, GS_TRXPOS, 0);
    gs_write(&gs, GS_TRXREG, WIDTH | (uint64_t)HEIGHT << 32);
    gs_write(&gs, GS_TRXDIR, 2);
    show(256, 0x8000FF00, "new draw target then local copy");

    clear(128, 0xA500FF00);
    show(128, 0xA500FF00, "colour before high-byte uploads");
    gs_write_pixel(&gs, GS_PSMT4HL, 4096, 8, 7, 9, 3);
    gs_write_pixel(&gs, GS_PSMT4HH, 4096, 8, 8, 9, 4);
    gs_write_pixel(&gs, GS_PSMT8H, 4096, 8, 9, 9, 0x7E);
    gs_write_pixel(&gs, GS_PSMT4HL, 4096, 8, 10, 9, 0);
    show(128, 0xA500FF00, "high-byte uploads preserve every RGB pixel");
    sample_alpha(7, 9, 0xA3, "4HL preserves the high nibble");
    sample_alpha(8, 9, 0x45, "4HH preserves the low nibble");
    sample_alpha(9, 9, 0x7E, "8H replaces the byte");
    sample_alpha(10, 9, 0xA0, "zero nibble is still written");
    sample_alpha(0, 0, 0xA5, "partial uploads preserve other pixels");

    clear(56, 0x8000FF00);
    show(56, 0x8000FF00, "first loading buffer before font upload");
    clear(112, 0x800000FF);
    show(112, 0x800000FF, "second loading buffer before font upload");
    gs_write(&gs, GS_BITBLTBUF, 128ULL << 32 | 8ULL << 48 | (uint64_t)GS_PSMT4HL << 56);
    gs_write(&gs, GS_TRXPOS, 0);
    gs_write(&gs, GS_TRXREG, 512ULL | 512ULL << 32);
    gs_write(&gs, GS_TRXDIR, 0);
    for (unsigned i = 0; i < 512 * 512 / 16; i++) gs_write(&gs, GS_HWREG, 0xFFFFFFFFFFFFFFFFULL);
    show(56, 0x8000FF00, "font alpha preserves the whole first loading buffer");
    show(112, 0x800000FF, "font alpha preserves the second loading buffer's corner");

#ifdef DHD_NATIVE_VULKAN
    backend_cases();
#endif
    gs_gpu_destroy(&gs);
#ifndef DHD_NATIVE_VULKAN
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
#endif
    return failures ? 1 : 0;
}
