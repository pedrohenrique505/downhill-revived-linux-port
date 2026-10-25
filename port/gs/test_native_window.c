/* Synthetic SDL native window: the shared render thread, movies, resize and interpolation.
   Reuse the no-game-data GS helpers/stubs of the transfer test. */
#define main transfer_test_main
#include "test_native_transfers.c"
#undef main
#include <SDL3/SDL.h>

int main(void)
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return 2;
    SDL_Window *window = SDL_CreateWindow("DHD native window test", 640, 480,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) { fprintf(stderr, "%s\n", SDL_GetError()); return 2; }
    gs_reset(&gs);
    if (!gs_vulkan_init(&gs) || !gs_vulkan_native_window(&gs, window)) return 2;
    Native *n = native_of(&gs);
    gs.source_path = 1;
    double start = host_wall_seconds(), epoch_start = 0;
    uint64_t epoch_shown = 0;
    uint32_t movie[32*24];
    for (unsigned i = 0; i < 32*24; i++) movie[i] = 0xFF123456;
    for (unsigned field = 0; field < 150; field++) {
        if (field == 30) {
            gs_native_bench(&gs, 0);
            if (n->interp) {
                native_context_enter(n);
                uint64_t previous = n->stats.presented;
                native_context_leave(n);
                /* Independent wall/count observation of the first picture in the new epoch. */
                do {
                    SDL_Delay(1);
                    native_context_enter(n);
                    epoch_shown = n->stats.presented;
                    epoch_start = host_wall_seconds();
                    native_context_leave(n);
                } while (epoch_shown == previous);
            }
            SDL_SetWindowSize(window, 960, 540);
        }
        if (field == 60) SDL_MinimizeWindow(window);
        if (field == 65) SDL_RestoreWindow(window);
        if (field == 90) {
            gs_native_bench(&gs, 1);
            SDL_SetWindowSize(window, 600, 700);
            setenv("DHD_PRESENT_FILTER", "point", 1);
            setenv("DHD_ASPECT", "16:9", 1);
            gs_native_present_options(&gs);
        }
        SDL_PumpEvents();
        SDL_Event event; while (SDL_PollEvent(&event)) {}
        int w = 0, h = 0;
        if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) SDL_GetWindowSizeInPixels(window, &w, &h);
        native_vulkan_window_size(n, w, h);
        clear(56, 0x800000FFu | (field << 8));
        gs_priv_write(&gs, GS_PMODE, 2);
        gs_priv_write(&gs, GS_SMODE2, 0);
        gs_priv_write(&gs, GS_DISPFB2, 56 | 8ULL << 9);
        gs_priv_write(&gs, GS_DISPLAY2, (WIDTH-1ULL)<<32 | (HEIGHT-1ULL)<<44);
        gs_native_movie(&gs, field >= 105 && field < 120 ? movie : NULL, 32, 24);
        unsigned width = 0, height = 0;
        kernel_vsync_count++;
        native_vsync(&gs, window, &width, &height, 0);
        if (field == 110 || field == 130) {
            uint32_t *picture = gs_native_picture(&gs, &width, &height);
            if (!picture || (field == 110 && (width != 32 || height != 24 || picture[0] != movie[0]))) {
                fprintf(stderr, "FAIL: native movie/readback\n"); return 1;
            }
            if (field == 110) {
                /* Screenshot formatting must not change capture.c's raw display dimensions. */
                setenv("DHD_SCREENSHOT_WINDOW", "320x200", 1);
                picture = gs_native_picture(&gs, &width, &height);
                if (!picture || width != 320 || height != 200) return 1;
                uint32_t raw[32*24];
                if (!native_capture_display(n, &width, &height) || width != 32 || height != 24 ||
                    !native_capture(n, raw) || raw[0] != movie[0]) return 1;
                unsetenv("DHD_SCREENSHOT_WINDOW");
            }
        }
        double due = start + (field+1)/60.0;
        while (host_wall_seconds() < due) SDL_Delay(1);
    }
    /* Wait for the final handed frame, then stop/join before releasing SDL. */
    unsigned w, h; if (!gs_native_picture(&gs, &w, &h)) return 1;
    native_context_enter(n);
    unsigned long long frames = n->stats.frames, shown = n->stats.presented, mixed = n->stats.interp_frames;
    double observed_fps = epoch_start ? (shown - epoch_shown) / (host_wall_seconds() - epoch_start) : 0;
    native_context_leave(n);
    int ok = frames == 150 && shown >= 120 && (!n->interp_hz || mixed > 10);
    fprintf(stdout, "window: frames=%llu presented=%llu interpolated=%llu observed_fps=%.2f %s\n",
            frames, shown, mixed, observed_fps, ok ? "PASS" : "FAIL");
    gs_gpu_destroy(&gs);
    SDL_DestroyWindow(window); SDL_Quit();
    return ok ? 0 : 1;
}
