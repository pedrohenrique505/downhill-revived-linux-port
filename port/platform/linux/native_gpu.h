/* Offscreen native Vulkan backend. Presentation is supplied separately by the host. */
#ifndef DHD_LINUX_NATIVE_GPU_H
#define DHD_LINUX_NATIVE_GPU_H
#include "../../gs/native.h"
int gs_vulkan_native(Gs *g);
void native_vulkan_destroy(Native *n);
#endif
