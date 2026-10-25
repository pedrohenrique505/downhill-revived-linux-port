/* SDL window/input belong to the EE thread. The native renderer owns Vulkan
   presentation on its render thread; exact uses SDL's Vulkan renderer. */
#include <SDL3/SDL.h>
#include "sdl_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "present.h"
#include "gpu.h"
#include "native_gpu.h"
#include "../../gs/gpu.h"
#include "capture.h"
#include "../hle/hle.h"
#include "../hle/text_input.h"

volatile uint32_t gs_presented_frames;
volatile int gs_native_view;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static unsigned texture_width, texture_height;
static uint32_t pixels[1024 * 1024];
static int initialized, disabled, text_on;
static int native_selected;

const uint32_t *movie_picture(unsigned *width, unsigned *height);  /* port/hle/movie.c */

/* CPU scanout of synchronized Vulkan VRAM; capture.c uses gs_display. */
int gs_gpu_display(Gs *g, unsigned *width, unsigned *height)
{
    if (!gs_gpu_native(g)) return 0;
    unsigned w, h;
    if (!gs_native_picture(g, &w, &h) || (size_t)w*h > 1024*1024) return 0;
    *width = w; *height = h; return 1;
}

int gs_gpu_capture(Gs *g, uint32_t *out)
{
    return gs_gpu_native(g) && native_capture(native_of(g), out);
}

static void window_size(int *w, int *h)
{
    const char *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    *w = width && atoi(width) > 0 ? atoi(width) : 896;
    *h = height && atoi(height) > 0 ? atoi(height) : 672;
}

void gs_window_apply(void)
{
    if (!window) return;
    const char *mode = getenv("DHD_WINDOW");
    int full = mode && (!strcmp(mode, "borderless") || !strcmp(mode, "fullscreen"));
    if (full) {
        /* borderless: the desktop's own mode; fullscreen: the same here (no exclusive mode change) */
        SDL_SetWindowFullscreenMode(window, NULL);
        SDL_SetWindowFullscreen(window, true);
    } else {
        int w, h;
        window_size(&w, &h);
        SDL_SetWindowFullscreen(window, false);
        SDL_SetWindowSize(window, w, h);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

void gs_present_options_apply(void)
{
    if (gs_gpu_native(&gs)) gs_native_present_options(&gs);
}

static void shutdown(void)
{
    gs_gpu_destroy(&gs); /* joins native presentation before the SDL window disappears */
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    texture = NULL; renderer = NULL; window = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static int init(void)
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "gs: SDL video didn't start: %s\n", SDL_GetError());
        return 0;
    }
    int w, h;
    window_size(&w, &h);
    window = SDL_CreateWindow("Downhill Revived", w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (native_selected ? SDL_WINDOW_VULKAN : 0));
    if (!window) {
        fprintf(stderr, "gs: no window: %s\n", SDL_GetError());
        return 0;
    }
    gs_window_apply();
    if (native_selected) {
        if (!gs_vulkan_native_window(&gs, window)) return 0;
        hle_log("gs: SDL %s native Vulkan window\n", SDL_GetCurrentVideoDriver());
        return 1;
    }
    renderer = SDL_CreateRenderer(window, "vulkan");
    if (!renderer) {
        fprintf(stderr, "gs: no renderer: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetRenderVSync(renderer, 0);  /* the kernel paces the fields */
    hle_log("gs: SDL %s window, %s renderer\n", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(renderer));
    gs_window_apply();
    return 1;
}

/* kernel_init calls this after the exact Z backend and before any game packets. */
int gs_linux_present_init(void)
{
    if (initialized) return 1;
    initialized = 1;
    disabled = getenv("DHD_HEADLESS") != NULL;
    const char *render = getenv("DHD_RENDER");
    native_selected = render && !strcmp(render, "native");
    atexit(shutdown); /* graphics_queue registers later and drains first */
    if (disabled) return !native_selected || gs_vulkan_native(&gs);
    return init();
}

static void publish_size(void)
{
    if (!window || !gs_gpu_native(&gs)) return;
    int w = 0, h = 0;
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) SDL_GetWindowSizeInPixels(window, &w, &h);
    native_vulkan_window_size(native_of(&gs), w > 0 ? (unsigned)w : 0, h > 0 ? (unsigned)h : 0);
}

static int resize(unsigned width, unsigned height)
{
    if (texture && width == texture_width && height == texture_height) return 1;
    if (texture) SDL_DestroyTexture(texture);
    /* R, G, B and the GS's alpha in memory: the alpha is left out (XBGR8888 on little-endian), or
       the desktop's compositor would take it for the window's transparency */
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
    if (!texture) {
        fprintf(stderr, "gs: texture %ux%u: %s\n", width, height, SDL_GetError());
        return 0;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    texture_width = width, texture_height = height;
    hle_log("gs: scanout %ux%u DISPFB=%llX DISPLAY=%llX\n", width, height,
            gs_priv_read(&gs, GS_DISPFB1), gs_priv_read(&gs, GS_DISPLAY1));
    return 1;
}

/* The picture at 4:3 (the PS2's shape), centred, black around it. */
static void present(void)
{
    int w, h;
    SDL_GetRenderOutputSize(renderer, &w, &h);
    float fw = (float)w, fh = (float)h, pw = fw, ph = fw * 3.0f / 4.0f;
    if (ph > fh) ph = fh, pw = fh * 4.0f / 3.0f;
    SDL_FRect dst = {(fw - pw) / 2, (fh - ph) / 2, pw, ph};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, &dst);
    SDL_RenderPresent(renderer);
}

void gs_window_title(const char *utf8)
{
    if (window) SDL_SetWindowTitle(window, utf8);
}

/* EXIT GAME (port/hle/exit_menu.c): as the window's close button. */
void gs_window_close(void)
{
    if (!window) exit(0);
    SDL_Event quit = {.type = SDL_EVENT_QUIT};
    SDL_PushEvent(&quit);
}

/* The UTF-8 of a text-input event, a character at a time as Windows' WM_CHAR gives them. */
static void text_characters(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3Fu >> extra;
        while (extra-- > 0 && *p) c = c << 6 | (*p++ & 0x3Fu);
        if (c < 0x10000) text_input_character(c);
    }
}

void gs_pump_messages(void)
{
    SDL_Event e;
    if (!window) return;
    int want_text = text_input_active();
    if (want_text != text_on) {
        if (want_text) SDL_StartTextInput(window);
        else SDL_StopTextInput(window);
        text_on = want_text;
    }
    SDL_PumpEvents();
    while (linux_window_event(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            exit(0);
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            text_input_clear();
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (text_on) text_characters(e.text.text);
            break;
        case SDL_EVENT_KEY_DOWN:
            /* the editing keys Windows sends as characters too */
            if (text_on && e.key.key == SDLK_BACKSPACE) text_input_character(8);
            if (text_on && e.key.key == SDLK_RETURN) text_input_character(13);
            break;
        default:
            break;
        }
    }
    publish_size();
}

/* The keyboard by Windows virtual-key code, for port/platform/linux/host.c (keyboard.c's keys:
   letters by their place on a US keyboard, as SDL's scancodes are). Only while the game's window
   has the keyboard, and not with Alt, Ctrl or the system key held. */
int host_keyboard_state(uint8_t down[256])
{
    if (!window || SDL_GetKeyboardFocus() != window) return 0;
    if (SDL_GetModState() & (SDL_KMOD_ALT | SDL_KMOD_CTRL | SDL_KMOD_GUI)) return 0;
    const bool *keys = SDL_GetKeyboardState(NULL);
    static const struct { uint8_t vk; SDL_Scancode code; } named[] = {
        {0x08, SDL_SCANCODE_BACKSPACE}, {0x09, SDL_SCANCODE_TAB}, {0x0D, SDL_SCANCODE_RETURN},
        {0x0D, SDL_SCANCODE_KP_ENTER}, {0x1B, SDL_SCANCODE_ESCAPE}, {0x20, SDL_SCANCODE_SPACE},
        {0x25, SDL_SCANCODE_LEFT}, {0x26, SDL_SCANCODE_UP}, {0x27, SDL_SCANCODE_RIGHT}, {0x28, SDL_SCANCODE_DOWN},
    };
    for (unsigned i = 0; i < sizeof named / sizeof *named; i++)
        if (keys[named[i].code]) down[named[i].vk] = 1;
    for (int letter = 0; letter < 26; letter++)
        if (keys[SDL_SCANCODE_A + letter]) down['A' + letter] = 1;
    return 1;
}

/* DHD_BENCH=first-last times the wall clock between consecutive fields of that virtual-field
   window and prints one summary line at the last field, with each port thread's CPU time and
   DHD_TIMELINE's waits (graphics_queue.c); DHD_BENCH_EXIT=1 then ends the process. As
   port/gs/present.c, without the native renderer's and the sound's counters. */
void graphics_timeline(int phase, unsigned field, double wall_seconds);

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void bench_threads(int end, unsigned fields)
{
    enum { MAX = 16 };
    static const char *names[MAX];
    static double before[MAX];
    static unsigned count;
    double now[MAX];
    const char *now_names[MAX];
    unsigned n = host_thread_cpu(now_names, now, MAX);
    if (!end) {
        count = n;
        for (unsigned i = 0; i < n; i++) names[i] = now_names[i], before[i] = now[i];
        return;
    }
    fprintf(stderr, "bench threads: cpu_ms_per_field");
    for (unsigned i = 0; i < n; i++) {
        double start = 0;
        for (unsigned j = 0; j < count; j++) if (names[j] == now_names[i]) start = before[j];
        fprintf(stderr, " %s=%.3f", now_names[i], (now[i] - start) * 1e3 / fields);
    }
    fprintf(stderr, "\n");
}

static int grab(int gpu_frame, const uint32_t **image, unsigned *width, unsigned *height)
{
    if (gs_gpu_native(&gs)) return (*image = gs_native_picture(&gs, width, height)) != NULL;
    *image = pixels;
    return !gpu_frame || gs_gpu_capture(&gs, pixels);
}

static void capture(unsigned width, unsigned height, int gpu_frame)
{
    const uint32_t *image = pixels;
    static unsigned saved;
    const char *path = getenv("DHD_SCREENSHOT"), *at = getenv("DHD_CAPTURE_AT");
    const char *every = getenv("DHD_CAPTURE_INTERVAL");
    const char *frame_at = getenv("DHD_CAPTURE_FRAME");
    extern uint64_t kernel_vsync_count;
    double capture_time = frame_at ? (double)kernel_vsync_count : host_seconds();
    if (frame_at) {
        at = frame_at;
        every = getenv("DHD_CAPTURE_FRAME_INTERVAL");
    }
    double interval = every ? atof(every) : 0;
    char numbered[1024];
    race_sync_vsync(kernel_vsync_count);
    /* DHD_CAPTURE_FIELDS=first-last[,first-last...]: every field of those windows, PATH-fNNNNNN.bmp. */
    const char *fields = getenv("DHD_CAPTURE_FIELDS");
    if (path && fields) {
        unsigned long long first, last;
        int hit = 0;
        for (const char *s = fields; s && sscanf(s, "%llu-%llu", &first, &last) == 2; s = strchr(s, ','), s = s ? s + 1 : s)
            hit |= kernel_vsync_count >= first && kernel_vsync_count <= last;
        if (!hit || !grab(gpu_frame, &image, &width, &height)) return;
        snprintf(numbered, sizeof numbered, "%s-f%06llu.bmp", path, (unsigned long long)kernel_vsync_count);
        path = numbered;
    } else if (getenv("DHD_RACE_SYNC")) {
        if (!path) return;
        if (!race_sync_capture(path, numbered, sizeof numbered)) {
            /* Survey-only periodic pictures alongside the named race windows.
               DHD_CAPTURE_FRAME is otherwise intentionally ignored in race
               mode. Named consecutive captures always have priority. */
            const char *period = getenv("DHD_RACE_CAPTURE_INTERVAL");
            if (!period || strtoul(period, NULL, 10) == 0) return;
            static unsigned long long next;
            unsigned long long step = strtoul(period, NULL, 10);
            const char *first = getenv("DHD_RACE_CAPTURE_FROM");
            if (!next) next = first ? strtoull(first, NULL, 10) : step;
            if (kernel_vsync_count < next) return;
            next = (kernel_vsync_count / step + 1) * step;
            snprintf(numbered, sizeof numbered, "%s-f%06llu.bmp", path, (unsigned long long)kernel_vsync_count);
        }
        if (!grab(gpu_frame, &image, &width, &height)) return;
        path = numbered;
    } else {
        if (!path || (saved && interval <= 0) ||
            capture_time < (at ? atof(at) : 10.0) + saved * interval) return;
        if (!grab(gpu_frame, &image, &width, &height)) return;
        if (interval > 0) {
            snprintf(numbered, sizeof numbered, "%s-%03u.bmp", path, saved);
            path = numbered;
        }
        saved++;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "gs: cannot save screenshot %s\n", path); return; }
    BITMAPFILEHEADER file = {0};
    BITMAPINFOHEADER info = {0};
    file.bfType = 0x4D42; file.bfOffBits = sizeof file + sizeof info;
    file.bfSize = file.bfOffBits + width * height * 4;
    info.biSize = sizeof info; info.biWidth = width; info.biHeight = -(LONG)height;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    fwrite(&file, sizeof file, 1, f); fwrite(&info, sizeof info, 1, f);
    for (unsigned i = 0; i < width * height; i++) {
        uint32_t v = image[i], bgra = (v & 0xFF00FF00) | (v & 255) << 16 | (v >> 16 & 255);
        fwrite(&bgra, 4, 1, f);
    }
    fclose(f);
    fprintf(stderr, "compare: capture %s field=%llu host=%.6f game=%.6f\n", path,
            (unsigned long long)kernel_vsync_count, host_wall_seconds(), host_seconds());
    hle_log("gs: saved %s (%llu PATH1 / %llu PATH2 / %llu PATH3 packets, %llu kicks, %llu uploads)\n",
            path, gs.packets[0], gs.packets[1], gs.packets[2], gs.kicks, gs.uploads);
    hle_log("gs: PMODE=%llX SMODE2=%llX DISPFB1=%llX DISPLAY1=%llX DISPFB2=%llX DISPLAY2=%llX\n",
            gs_priv_read(&gs, GS_PMODE), gs_priv_read(&gs, GS_SMODE2),
            gs_priv_read(&gs, GS_DISPFB1), gs_priv_read(&gs, GS_DISPLAY1),
            gs_priv_read(&gs, GS_DISPFB2), gs_priv_read(&gs, GS_DISPLAY2));
}

static void bench_vsync(void)
{
    static int state;  /* 0 unread, 1 armed, 2 done */
    static unsigned long long first, last;
    static double start, previous, *frames;
    extern uint64_t kernel_vsync_count;
    if (state == 2) return;
    if (!state) {
        const char *range = getenv("DHD_BENCH");
        if (!range || sscanf(range, "%llu-%llu", &first, &last) != 2 || last <= first) { state = 2; return; }
        frames = calloc(last - first, sizeof *frames);
        if (!frames) { state = 2; return; }
        state = 1;
    }
    unsigned long long field = kernel_vsync_count;
    if (field < first) return;
    double now = host_wall_seconds();
    if (field == first) {
        start = previous = now;
        bench_threads(0, 0);
        gs_native_bench(&gs, 0);
        graphics_timeline(0, (unsigned)field, 0);
        return;
    }
    if (field > last) { state = 2; return; }  /* started inside the window: no clean measurement */
    frames[field - first - 1] = now - previous;
    graphics_timeline(field < last ? 1 : 2, (unsigned)field, now - previous);
    previous = now;
    if (field < last) return;
    unsigned n = (unsigned)(last - first), worst = n / 100 ? n / 100 : 1;
    gs_native_bench(&gs, 1);
    bench_threads(1, n);
    qsort(frames, n, sizeof *frames, compare_double);
    double tail = 0;
    for (unsigned i = n - worst; i < n; i++) tail += frames[i];
    fprintf(stderr, "bench: fields=%u-%llu fields_per_s=%.3f p50_ms=%.3f p99_ms=%.3f worst1pct_ms=%.3f max_ms=%.3f\n",
            (unsigned)first, last, n / (now - start), frames[n / 2] * 1e3, frames[n * 99 / 100] * 1e3,
            tail / worst * 1e3, frames[n - 1] * 1e3);
    fflush(stderr);
    free(frames);
    state = 2;
    const char *quit = getenv("DHD_BENCH_EXIT");
    if (quit && !strcmp(quit, "1")) exit(0);
}

void gs_vsync(void)
{
    static unsigned previous_bp = ~0u;
    if (!initialized && !gs_linux_present_init()) {
        fprintf(stderr, "Downhill Revived couldn't start its graphics (details above).\n"); exit(1);
    }
    bench_vsync();
    if (disabled && !native_selected) return;
    gs_pump_messages();
    /* Plain MMIO pages are the authoritative privileged register storage. */
    for (unsigned addr = 0; addr <= 0xE0; addr += 16)
        gs_priv_write(&gs, addr, *(volatile uint64_t *)(uintptr_t)(0x12000000 + addr));
    uint64_t pmode = gs_priv_read(&gs, GS_PMODE);
    unsigned bp = (unsigned)gs_priv_read(&gs, pmode & 1 ? GS_DISPFB1 : GS_DISPFB2) & 511;
    if ((pmode & 3) && bp != previous_bp) { gs_presented_frames++; previous_bp = bp; }
    unsigned width = 640, height = 448;
    /* While a movie plays (port/hle/movie.c) its picture is shown instead. */
    unsigned movie_width, movie_height;
    const uint32_t *movie = movie_picture(&movie_width, &movie_height);
    if (gs_gpu_native(&gs)) {
        gs_native_movie(&gs, movie, movie ? movie_width : 0, movie ? movie_height : 0);
        native_vsync(&gs, window, &width, &height, 0);
    } else {
        if (movie) {
            width = movie_width, height = movie_height;
            memcpy(pixels, movie, (size_t)width * height * 4);
        } else if (!gs_display(&gs, pixels, &width, &height)) {
            memset(pixels, 0, (size_t)width * height * 4);
        }
        if (!resize(width, height)) return;
        SDL_UpdateTexture(texture, NULL, pixels, (int)width * 4);
        double due, began;
        pacing_handed(&due, &began);
        pacing_present(due, began);
        present();
    }
    capture(width, height, 0);
    extern uint64_t kernel_vsync_count;
    if (getenv("DHD_RACE_SYNC") && getenv("DHD_RACE_GSDUMP")) {
        char name[1024];
        /* The shared race-clock scheduler requests the GS dump as on Windows.
           capture.c saves the matching native pictures with that dump. */
        race_sync_capture("", name, sizeof name);
    }
    gs_capture_vsync(&gs);
}
