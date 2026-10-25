/* port/gs/present.c on Linux: the SDL3 window and GS framebuffer scanout.

   gs.c decodes GIF packets and transfers; gs_display decodes the selected display circuit into
   RGBA8, and SDL's Vulkan renderer scales it into the window at 4:3. The Vulkan
   compute backend (gpu.c), installed before the game starts, rasterizes into
   coherent GS VRAM. Scanout and captures synchronize through gs_display.
   Frame pacing is the kernel's
   (pacing_handed/pacing_present), as on Windows. The window's events are handled on the game thread,
   once a field: SDL wants them on the thread that made the window. */
#include <SDL3/SDL.h>
#include "sdl_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "present.h"
#include "gpu.h"
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

const uint32_t *movie_picture(unsigned *width, unsigned *height);  /* port/hle/movie.c */

/* CPU scanout of synchronized Vulkan VRAM; capture.c uses gs_display. */
int gs_gpu_display(Gs *g, unsigned *width, unsigned *height)
{
    (void)g, (void)width, (void)height;
    return 0;
}

int gs_gpu_capture(Gs *g, uint32_t *out)
{
    (void)g, (void)out;
    return 0;
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

void gs_present_options_apply(void) {}  /* the picture's filter and shape: the native renderer's */

static int init(void)
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "gs: SDL video didn't start: %s\n", SDL_GetError());
        return 0;
    }
    int w, h;
    window_size(&w, &h);
    window = SDL_CreateWindow("Downhill Revived", w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        fprintf(stderr, "gs: no window: %s\n", SDL_GetError());
        return 0;
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

void gs_vsync(void)
{
    static unsigned previous_bp = ~0u;
    if (!initialized) {
        initialized = 1;
        disabled = getenv("DHD_HEADLESS") != NULL;
        if (!disabled && !init()) {
            fprintf(stderr, "Downhill Revived couldn't start its graphics (details above).\n");
            exit(1);
        }
    }
    if (disabled) return;
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
    gs_capture_vsync(&gs);
}
