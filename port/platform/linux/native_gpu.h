/* Offscreen native Vulkan backend. Presentation is supplied separately by the host. */
#ifndef DHD_LINUX_NATIVE_GPU_H
#define DHD_LINUX_NATIVE_GPU_H
#include "../../gs/native.h"
typedef struct SDL_Window SDL_Window;
int gs_vulkan_native(Gs *g);
int gs_vulkan_native_window(Gs *g, SDL_Window *window);
void native_vulkan_window_size(Native *n, unsigned width, unsigned height);
void native_vulkan_destroy(Native *n);
#endif
