/* The native renderer (DHD_RENDER=native): what its portable frontend (native.c) and a graphics
   API's backend (gpu_native.inc: Direct3D 11) share. No graphics API header is needed here.

   The frontend records each field on the GS worker as commands over surfaces and decoded
   textures, keeps the exact Z probe with the exact backend, and runs the render thread; the
   backend makes the images, runs a recorded field's commands, scans out, presents and reads the
   picture back (native_gpu_*). */
#ifndef PORT_GS_NATIVE_H
#define PORT_GS_NATIVE_H
#include <windows.h>  /* CRITICAL_SECTION and friends (port/platform/linux/include on Linux) */
#include <stdio.h>
#include "gs.h"
#include "texpack.h"
#include "native_frontend.h"

struct Native;

/* The first member of an exact backend's state (g->backend) that hosts the native renderer: the
   renderer finds itself there, and while it runs the exact backend clips every primitive to clip
   (inclusive pixels), so it draws only what the Z probe needs. */
typedef struct {
    struct Native *native;
    int clip_on, clip[4];
} NativeHost;

static inline struct Native *native_of(const Gs *g) { return ((const NativeHost *)g->backend)->native; }

enum { NATIVE_SURFACES = 48, NATIVE_TEXTURES = 4096, NATIVE_WATCH = 128, NATIVE_CONVERTS = 64 };

typedef struct {
    float x, y, z, s, t, q, fog;
    uint32_t rgba;
} NativeVertex;

typedef struct {
    float target[2]; uint32_t flags, atst;
    uint32_t aref; float alpha_scale, texa_alpha;
    uint32_t scale;                    /* set when drawn: target, texture, sampled scale (raster.hlsl) */
    float fog[4];
    int32_t tex_offset[2]; uint32_t tex_size[2];
    uint32_t wrap[4];
    uint32_t region[4];
} NativeConstants;

/* A backend's image (D3D11: a texture) and the view a draw samples it through (D3D11: a shader
   resource view). */
typedef struct NativeImage NativeImage;
typedef struct NativeView NativeView;

typedef struct {
    uint64_t key;
    NativeImage *texture;
    NativeView *view;
    uint64_t used;        /* last field that drew with it */
    TexPackEntry *pack;   /* its replacement (texpack.c), once looked up (pack_checked) */
    int pack_checked;
    NativeView *sharp;    /* the game's font made sharp (font_sharp.c), or NULL */
} NativeTexture;

/* A render target or depth buffer: GS memory from page `page`, `bw` pages wide. */
typedef struct {
    unsigned page, bw, wide, depth;    /* wide: 16-bit pixel layout (64x64 pages) */
    unsigned zpsm;                     /* depth: the Z format it was last drawn with */
    volatile LONG extent[2];           /* pixels claimed so far (GS worker; only grows) */
    unsigned scale, size[2];           /* render thread: its resolution scale; texture size (scaled pixels, 0: none yet) */
} NativeSurface;

enum { NATIVE_DRAW, NATIVE_COPY, NATIVE_IMPORT, NATIVE_ALPHA_UPLOAD };
typedef struct {
    uint8_t kind, depth_key, color_mask, pad;
    int16_t target, depth, texture_surface;
    int16_t convert;                   /* the frame's texture conversion this draw samples, or -1 */
    uint32_t blend_key, blend_fix;
    NativeTexture *texture;
    NativeView *replacement;           /* texpack.c: drawn instead of texture (flag 2048) */
    RECT scissor;
    NativeConstants constants;
    uint32_t first, count;
    /* NATIVE_COPY: source surface rectangle to (x, y) in target; NATIVE_IMPORT: pixels. */
    int16_t source;
    int x, y, w, h, sx, sy;
    uint32_t *pixels;
} NativeCommand;

/* DHD_INTERP pairs vertices by segment: one GIF tag's primitives (a VU1 batch, a piece of a
   mesh), under one draw state. Draw commands merge many of them, and culling or clipping in
   any one changes the command's vertex count. */
typedef struct {
    uint32_t first, count, key;      /* key: a hash of the command's draw state */
} NativeSegment;

/* A texture the GPU rebuilds from a surface just before the draw that samples it (native_convert
   in raster.hlsl); the constants match its cbuffer. */
typedef struct {
    uint32_t tex[4], size[4];
    uint32_t slot[4][4];               /* per source surface: page, bw, 1 for depth, Z units (float bits) */
    uint8_t pages[512];                /* per GS page: 0 the CPU copy (the decoded base), 1-4 a source slot */
    uint32_t palette[256];
} NativeConvertConstants;
typedef struct {
    int16_t surface[4];
    NativeTexture *base;               /* the texture decoded from the CPU copy, or NULL */
    NativeConvertConstants constants;
} NativeConvert;

typedef struct {
    NativeVertex *vertices;
    unsigned vertex_count, vertex_capacity;
    NativeCommand *commands;
    unsigned command_count, command_capacity;
    NativeConvert *converts;
    unsigned convert_count, convert_capacity;
    int display_surface;
    uint32_t display[12];
    uint32_t *movie;                 /* a movie's picture shown instead (gs_native_movie), when movie_on */
    unsigned movie_width, movie_height, movie_capacity;
    int movie_on;
    uint64_t field;
    double time;                     /* host wall seconds at vsync */
    double due, began;               /* when to present it (0: at once), when its pad was read (kernel.c) */
    NativeSegment *segments;         /* DHD_INTERP: the vertices of each GIF tag */
    unsigned segment_count, segment_capacity;
    int32_t *pair;                   /* DHD_INTERP: each segment's partner in the older field, or -1 */
    unsigned pair_capacity;
    unsigned path1;                  /* primitives VU1 drew (PATH1): 0 in a field of 2D only */
    unsigned flat_limit;             /* loading state at handoff, never read from the render thread */
} NativeFrame;

/* Primitives that write Z buffer memory, for the exact probe answer: Z writes (zbp) and colour
   writes into a Z buffer's pages (fbp; the game clears part of its Z buffer that way). */
enum { NATIVE_NO_PAGE = 0xFFFF };
typedef struct {
    unsigned type, count, state, zbp, fbp;
    int box[4];
    GsVertex v[3];
    uint8_t unconditional;
} NativeLogDraw;
typedef struct { uint64_t reg[128]; uint16_t clut[1024]; } NativeLogState;
typedef struct {
    NativeLogDraw *draws; unsigned draw_count, draw_capacity;
    NativeLogState *states; unsigned state_count, state_capacity;
    uint64_t state_version;
} NativeLog;

typedef struct {
    uint64_t draws, primitives, commands, decoded, decoded_texels, cache_hits, imports, copies;
    uint64_t texture_from_surface, texture_unsupported, blend_approximate, date_ignored;
    uint64_t probe_hits, probe_replays, probe_uncovered, probe_replayed_draws, exact_draws;
    uint64_t color_reads, copy_fallbacks, display_missing, frames, presented;
    uint64_t unsupported_why[6]; /* depth, format, width class, block offset, buffer width, layout */
    uint64_t interp_frames, paired_vertices, held_vertices, jumped_vertices, z_alias_draws;
    uint64_t band_draws, band_rt_textures, probe_band_hits, band_moves, target_draws, exact_targets, full_fields;
    uint64_t converted, conversions_run, blend_dest_alpha;
} NativeStats;

enum { NATIVE_FRAMES = 6, NATIVE_INTERVALS = 8192 };

/* DHD_NATIVE_SURFACES=1: how each surface is used, printed at exit. Sprites that sample a surface
   are binned by texels per pixel (<0.75, about 1, about 2, more) and, for the 1:1 ones, by the
   fraction of a texel between U and the pixel's X (quarters): which sampling the upscaled
   surfaces have to keep aligned. */
typedef struct {
    uint64_t draws, depth_draws, sampled, copied_in, copied_out, converted, displayed;
    int depth_with;
    uint64_t pair[NATIVE_SURFACES], ratio[NATIVE_SURFACES][4], offset[NATIVE_SURFACES][4];
} NativeUsage;

typedef struct Native {
    NativeExactCache exact_state;
    /* The exact backend: where its clip is, and its draw and staged reads (g->draw, g->stage_pages
       before the native renderer took them over). */
    NativeHost *host;
    void (*exact_draw)(Gs *g, unsigned prim, const GsVertex *vertices, unsigned count);
    void (*exact_stage_pages)(Gs *g, const uint32_t want[16], uint32_t staged[16]);
    void *gpu;                       /* the backend's state */
    CRITICAL_SECTION context_lock;   /* the backend's device context: render thread, captures, settings */
    volatile LONG context_waiters;     /* Linux: renderer yields the context to requested host operations */
    /* recording (GS worker) */
    NativeFrame frames[NATIVE_FRAMES];
    NativeFrame *recording;
    NativeCommand current;
    int current_valid;
    uint64_t state_version, written;
    unsigned state_ctxt, state_prim;
    float zscale; uint32_t zmax;
    int skip;                        /* the current state draws nothing */
    int box[4];                      /* pixels the open draw has touched */
    unsigned target_psm, depth_psm;
    int passes;                      /* AFAIL second pass needed */
    NativeCommand second;
    NativeSurface surfaces[NATIVE_SURFACES];
    volatile LONG surface_count;
    int16_t owner[512];
    uint32_t owner_writes[512];
    uint64_t upload_epoch, alpha_epoch[512];
    unsigned alpha_command[512];
    uint16_t color_pixel[2048];       /* GS word within a page -> row-major colour pixel */
    int16_t top_owner[512];          /* the colour surface that last drew a page, for a Z24 buffer's top byte */
    uint32_t top_writes[512];
    uint64_t page_hash[512];
    uint32_t page_hashed[512];
    uint8_t page_hash_valid[512];
    NativeTexture textures[NATIVE_TEXTURES];
    unsigned texture_count;
    int texpack;                       /* a texture pack is open (texpack.c) */
    uint64_t replaced_draws;
    int replacements;                  /* the backend can draw texture replacements and the sharp font */
    uint64_t field;
    /* exact Z probe */
    NativeLog logs[2];               /* previous field, this field */
    struct { int on, valid; unsigned zbp; int rect[4]; uint8_t covered[NATIVE_WATCH * NATIVE_WATCH]; unsigned missing; } watch;
    uint8_t zbw[512];                /* buffer width each Z buffer was last drawn with */
    uint8_t zbase[512];              /* pages used as a Z buffer base */
    uint8_t band[512];               /* pages kept exact all the time */
    uint16_t band_pages[64];
    uint8_t exact_target[512];       /* render target bases drawn exactly, whole */
    uint64_t full_until;             /* exact frames run through this field */
    unsigned full_hold;              /* fields they run on after the probe leaves the edge */
    unsigned band_count, band_key;   /* the Z buffer (base, width, rows) the band follows */
    int trace_probes;
    FILE *texlog;                    /* DHD_NATIVE_TEXLOG: textures drawn without their pixels */
    NativeUsage *usage;              /* DHD_NATIVE_SURFACES */
    uint64_t replay_version;
    /* render thread */
    HANDLE thread;
    CRITICAL_SECTION queue_lock;
    CONDITION_VARIABLE queue_changed;
    NativeFrame *pending[2], *free_frames[NATIVE_FRAMES];
    unsigned pending_count, free_count;
    uint64_t handed, rendered;
    int quit, resize;
    void *target;                    /* the backend's presentation target (D3D11: the swap chain) */
    float aspect;  /* the picture's shape in the window (DHD_ASPECT): width/height, 0 stretched, -1 its pixels */
    float wide;    /* widescreen's shape, or 0: fields of 2D only (loading screens) are shown 4:3 */
    unsigned flat_fields;            /* full-screen 2D fields in a row, up to the one shown */
    /* DHD_SCALE: wide surfaces (frame and Z buffers) are drawn this many times larger; 0 until
       "match window" has seen the window. Narrow ones (the shadow and its blur) stay at 1x. */
    unsigned scale_setting, scale;
    uint32_t *picture;                 /* DHD_SCREENSHOT of an upscaled picture */
    size_t picture_size;
    unsigned display_width, display_height;  /* the displayed picture (backend), 0 until there is one */
    NativeStats stats;
    /* DHD_INTERP */
    int interp;
    uint64_t segment_tags;           /* GIF tags seen when the open segment started */
    uint32_t segment_key;
    double interp_hz;
    double present_due, present_began;  /* the picture being drawn (NativeFrame due, began) */
    NativeFrame *older, *newer;      /* held by the render thread */
    NativeVertex *mixed;
    unsigned mixed_capacity;
    int32_t *same;                   /* native_mixed: segments by vertex positions (open addressing) */
    unsigned same_capacity;
    uint8_t *paired;
    unsigned paired_capacity;
    HANDLE timer;
    double last_present, first_present;
    float intervals[NATIVE_INTERVALS];   /* ms between presents, a ring */
    uint64_t interval_count;
    uint64_t bench_presented, bench_interp;  /* counters at the DHD_BENCH window's first field */
    /* DHD_NATIVE_TRACE=prefix: prefix-fields.csv, a line per recorded field (GS worker at vsync),
       and prefix-pictures.csv, a line per picture (render thread) with its GPU time. */
    FILE *trace_fields, *trace_pictures;
    NativeStats trace_last;
} Native;

/* A continuously presenting Linux thread can reacquire pthread's mutex ahead of
   already-waiting uploads, captures or settings forever. Publish those requests
   before waiting; the render thread lets them enter before starting another picture.
   Windows keeps its existing critical-section behaviour. */
static inline void native_context_enter(Native *n)
{
#ifdef __linux__
    InterlockedIncrement(&n->context_waiters);
#endif
    EnterCriticalSection(&n->context_lock);
#ifdef __linux__
    InterlockedDecrement(&n->context_waiters);
#endif
}
static inline void native_context_leave(Native *n) { LeaveCriticalSection(&n->context_lock); }
static inline void native_context_render_enter(Native *n)
{
#ifdef __linux__
    while (__atomic_load_n(&n->context_waiters, __ATOMIC_ACQUIRE)) Sleep(0);
#endif
    EnterCriticalSection(&n->context_lock);
}

/* Blend factors in a draw's blend_key (1 | source << 1 | destination << 6 | op << 11): GS blending
   as m * C + k, numbered as D3D11_BLEND and D3D11_BLEND_OP (op 1 add, 2 subtract, 3 reverse). */
enum { BLEND_ZERO = 1, BLEND_ONE = 2, BLEND_SA = 5, BLEND_ISA = 6, BLEND_DA = 7, BLEND_IDA = 8, BLEND_F = 14, BLEND_IF = 15 };

/* ---- the frontend (native.c), for the backends ---- */

/* After stopping the render thread and releasing backend resources: report and free frontend storage. */
void native_destroy(Native *n);
Native *native_create(void);          /* the renderer's state, before the backend's */
/* Once the backend is ready: takes over the GS hooks of the exact backend that host belongs to. */
void native_start(Gs *g, Native *n, NativeHost *host);
/* Hands the field to the render thread, to be presented in target (the backend's: D3D11 a swap
   chain). The display circuit's size goes to width and height. */
void native_vsync(Gs *g, void *target, unsigned *width, unsigned *height, int resize);
int native_present_options(Native *n);  /* DHD_PRESENT_FILTER and DHD_ASPECT; 0: the backend failed */
/* The picture's size in a w x h window (DHD_ASPECT, 2D fields at 4:3). */
void native_viewport(const Native *n, unsigned w, unsigned h, float *vw, float *vh);
void native_page_inverse(uint32_t inverse[4096]);  /* raster.hlsl's page-word-to-pixel table */
int native_capture_display(Native *n, unsigned *width, unsigned *height);
int native_capture(Native *n, uint32_t *pixels);   /* gs_gpu_capture */
void native_profile(Native *n);                    /* gs_gpu_profile, and at exit */

/* Host presentation and diagnostics, shared by Windows and Linux. */
int gs_gpu_native(const Gs *g);
void gs_native_lock(Gs *g, int lock);
void gs_native_present_options(Gs *g);
void gs_native_bench(Gs *g, int end);
uint32_t *gs_native_picture(Gs *g, unsigned *width, unsigned *height);
void gs_native_movie(Gs *g, const uint32_t *rgba, unsigned width, unsigned height);

/* ---- the backend (gpu_native.inc: Direct3D 11) ---- */

/* GS worker. A decoded texture (w x h RGBA) as t's image and view; with sharp, also the sharp
   font's view made from the same pixels (font_sharp.c, key names them). 0: nothing was made. */
int native_gpu_texture(Native *n, NativeTexture *t, const uint32_t *pixels, unsigned w, unsigned h, int sharp,
                       uint64_t key);
void native_gpu_texture_free(Native *n, NativeTexture *t);  /* a cache slot reused */
NativeView *native_gpu_pack_view(Native *n, TexPackEntry *e);  /* texpack_view: NULL until loaded */

/* Render thread, context lock held. */
unsigned native_gpu_window_height(Native *n);  /* the window's client height (DHD_SCALE=window), 0: unknown */
/* Surface s's image at want (GS pixels) times s->scale, grown if smaller, its contents kept;
   s->size becomes its size. 0: there is none. */
int native_gpu_surface(Native *n, NativeSurface *s, const unsigned want[2]);
void native_gpu_draw(Native *n, NativeFrame *f, const NativeVertex *vertices);  /* the field's commands */
int native_gpu_scanout(Native *n, NativeFrame *f);  /* the display circuit into the displayed picture */
int native_gpu_movie(Native *n, NativeFrame *f);    /* the field's movie picture as the displayed picture */
int native_gpu_present(Native *n);                  /* the displayed picture to the window; 0: not shown */
/* The displayed picture (display_width x display_height) into pixels. */
int native_gpu_read(Native *n, uint32_t *pixels);
void native_gpu_wait(Native *n);  /* before each picture: the presentation's frame latency */
int native_gpu_present_filter(Native *n, int point);
/* DHD_NATIVE_TRACE: GPU timestamps at a picture's start, once drawn and once presented (with its
   field, t and CPU time), written to n->trace_pictures when they arrive. */
enum { NATIVE_TIME_BEGIN, NATIVE_TIME_DRAWN, NATIVE_TIME_PRESENTED };
void native_gpu_time(Native *n, int stage, const NativeFrame *f, float t, float cpu_ms);

#endif
