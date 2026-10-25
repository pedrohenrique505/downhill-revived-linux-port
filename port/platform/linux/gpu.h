#ifndef DHD_LINUX_GPU_H
#define DHD_LINUX_GPU_H
#include "../../gs/gs.h"
/* Requires shaderFloat64 and a compute queue. Failure leaves callbacks unset. */
int gs_vulkan_init(Gs *g);
#endif
