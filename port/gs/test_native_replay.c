/* A version-8 GS capture (port/gs/replay.c's format) through DHD_RENDER=native: per field, the
   picture's size and FNV-1a hash (and a PPM of the first three), then the count and hash of the
   readbacks the capture asks for. Two builds that print the same lines drew the same pictures and
   answered the same reads (port/tools/test_native_replay.py). The presenter, texture
   replacements and EE scheduler are absent, as in test_native_transfers.c. */
#define COBJMACROS
#ifdef DHD_NATIVE_VULKAN
#include "../platform/linux/gpu.h"
#include "../platform/linux/native_gpu.h"
uint32_t *gs_native_picture(Gs *, unsigned *, unsigned *);
#else
#include "gpu_win32.h"
#endif
#include "present.h"
#include "gpu.h"
#include "texpack.h"
#include "font_sharp.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint64_t kernel_vsync_count;
volatile int gs_native_view;
double host_seconds(void) { return GetTickCount64() / 1000.0; }
double host_wall_seconds(void) { return host_seconds(); }
void host_thread_name(const char *n) { (void)n; }
void pacing_handed(double *d, double *b) { *d = *b = 0; }
void pacing_present(double d, double b) { (void)d; (void)b; }
int host_widescreen_flat_limit(void) { return 1000; }
double host_widescreen_aspect(void) { return 16.0 / 9; }
#ifndef DHD_NATIVE_VULKAN
UINT gs_swap_chain_flags(void) { return 0; }
void gs_frame_latency_wait(void) {}
HRESULT gs_present(IDXGISwapChain *s) { return IDXGISwapChain_Present(s, 0, 0); }
int texpack_open(ID3D11Device *d) { return 0; }
int texpack_available(void) { return 0; }
TexPackEntry *texpack_find(const Gs *g, unsigned c) { return NULL; }
ID3D11ShaderResourceView *texpack_view(TexPackEntry *e, uint64_t f) { return NULL; }
void texpack_field(uint64_t f) {}
void texpack_report(void) {}
int font_sharp_on(void) { return 0; }
int font_sharp_match(uint64_t t) { return 0; }
ID3D11ShaderResourceView *font_sharp_view(ID3D11Device *d, const uint32_t *p, unsigned w, unsigned h, uint64_t k) { return NULL; }

#else
int texpack_available(void) { return 0; }
TexPackEntry *texpack_find(const Gs *g, unsigned c) { (void)g; (void)c; return NULL; }
void texpack_field(uint64_t f) { (void)f; }
void texpack_report(void) {}
int font_sharp_on(void) { return 0; }
int font_sharp_match(uint64_t t) { (void)t; return 0; }
#endif

static FILE *input;
static void read_bytes(void *p, size_t size)
{
    if (fread(p, 1, size, input) != size) { fprintf(stderr, "Truncated GS dump\n"); exit(1); }
}
static uint32_t read32(void) { uint32_t v; read_bytes(&v, 4); return v; }
static uint64_t read64(void) { uint64_t v; read_bytes(&v, 8); return v; }
static void registers(void)
{
    uint8_t priv[8192]; read_bytes(priv, sizeof priv);
    for (unsigned i = 0; i < sizeof gs.privileged / sizeof *gs.privileged; i++) memcpy(gs.privileged + i, priv + i * 16, 8);
}
static void freeze(void)
{
    static const unsigned env[] = {GS_PRIM, GS_PRMODECONT, GS_TEXCLUT, GS_SCANMSK, GS_TEXA, GS_FOGCOL, GS_DIMX, GS_DTHE,
        GS_COLCLAMP, GS_PABE, GS_BITBLTBUF, GS_TRXDIR, GS_TRXPOS, GS_TRXREG, GS_TRXREG};
    static const unsigned context[] = {GS_XYOFFSET_1, GS_TEX0_1, GS_TEX1_1, GS_CLAMP_1, GS_MIPTBP1_1, GS_MIPTBP2_1,
        GS_SCISSOR_1, GS_ALPHA_1, GS_TEST_1, GS_FBA_1, GS_FRAME_1, GS_ZBUF_1};
    uint32_t crc = read32(), size = read32(), version = read32(); (void)crc;
    if (size != 4194752 || version != 8) { fprintf(stderr, "Expected native version-8 capture\n"); exit(1); }
    for (unsigned i = 0; i < sizeof env / sizeof *env; i++) gs.reg[env[i]] = read64();
    for (unsigned c = 0; c < 2; c++) for (unsigned i = 0; i < sizeof context / sizeof *context; i++) gs.reg[context[i] + c] = read64();
    uint64_t v = read64(); gs.vertex.rgba = (uint32_t)v;
    uint32_t q = (uint32_t)(v >> 32); memcpy(&gs.vertex.q, &q, 4);
    read_bytes(&gs.vertex.s, 8); v = read32(); gs.vertex.u = v & 16383; gs.vertex.v = v >> 16 & 16383;
    gs.vertex.fog = read32(); v = read64();
    gs.vertex.x = v & 65535; gs.vertex.y = v >> 16 & 65535; gs.vertex.z = (uint32_t)(v >> 32);
    read64(); read32(); read32(); read_bytes(gs.vram, GS_VRAM_SIZE);
    for (unsigned i = 0; i < 4; i++) {
        uint64_t tag = read64(), regs = read64(); unsigned reg = read32();
        if (i < 3) {
            GsPath *p = gs.path + i; p->left = tag & 32767; p->eop = tag >> 15 & 1;
            p->mode = tag >> 58 & 3; p->nreg = tag >> 60; if (!p->nreg) p->nreg = 16;
            p->regs = regs; p->reg = reg;
        }
    }
    read32(); registers();
    for (unsigned c = 0; c < 2; c++) gs_write(&gs, GS_TEX0_1 + c, gs.reg[GS_TEX0_1 + c]);
}
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
static void picture(const char *prefix, unsigned field)
{
    unsigned w = 0, h = 0;
    ++kernel_vsync_count;
#ifdef DHD_NATIVE_VULKAN
    native_vsync(&gs, NULL, &w, &h, 0);
#else
    gs_native_vsync(&gs, NULL, &w, &h, 0);
#endif
    uint32_t *pixels = gs_native_picture(&gs, &w, &h);
    if (!pixels) { printf("field %u: none\n", field); return; }
    printf("field %u: %ux%u %016llx\n", field, w, h, (unsigned long long)fnv(0xCBF29CE484222325ull, pixels, (size_t)w * h * 4));
    if (field >= 3 && !getenv("DHD_REPLAY_ALL")) return;
    char name[1024]; snprintf(name, sizeof name, "%s-%03u.ppm", prefix, field);
    FILE *f = fopen(name, "wb"); if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned i = 0; i < w * h; i++) {
        uint8_t rgb[3] = {(uint8_t)pixels[i], (uint8_t)(pixels[i] >> 8), (uint8_t)(pixels[i] >> 16)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: test_native_replay capture.gs output-prefix\n"); return 1; }
    input = fopen(argv[1], "rb"); if (!input) { perror(argv[1]); return 1; }
    _putenv("DHD_RENDER=native");
    gs_reset(&gs); freeze();
#ifdef DHD_NATIVE_VULKAN
    if (!gs_vulkan_init(&gs) || !gs_vulkan_native(&gs)) return 2;
#else
    ID3D11Device *device; ID3D11DeviceContext *context;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context)) ||
        !gs_gpu_init(&gs, device, context) || !gs_gpu_native(&gs)) return 2;
#endif
    unsigned fields = 0, reads = 0; int type;
    uint64_t readhash = 0xCBF29CE484222325ull;
    while ((type = fgetc(input)) != EOF) {
        if (type == 0) {
            unsigned path = fgetc(input); uint32_t size = read32();
            if (path < 1 || path > 3 || (size & 15)) return 1;
            uint32_t *packet = malloc(size); if (!packet) return 1;
            read_bytes(packet, size); gs_feed(&gs, path, packet, size / 4); free(packet);
        } else if (type == 1) {
            unsigned field = fgetc(input);
            gs.privileged[GS_CSR / 16] = (gs.privileged[GS_CSR / 16] & ~(UINT64_C(1) << 13)) | (uint64_t)field << 13;
            picture(argv[2], fields++);
        } else if (type == 2) {
            unsigned count = read32(); uint8_t *data = malloc((size_t)count * 16);
            if (!data) return 1;
            size_t bytes = gs_read_image(&gs, data, (size_t)count * 16);
            readhash = fnv(readhash, data, bytes); reads++;
            free(data);
        } else if (type == 3) registers();
        else { fprintf(stderr, "Unknown GS record %d\n", type); return 1; }
    }
    printf("readbacks %u %016llx\n", reads, (unsigned long long)readhash);
    gs_gpu_destroy(&gs);
    return 0;
}
