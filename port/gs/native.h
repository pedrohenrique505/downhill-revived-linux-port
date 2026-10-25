/* The native renderer (DHD_RENDER=native) as the GS backends see it. */
#ifndef PORT_GS_NATIVE_H
#define PORT_GS_NATIVE_H
#include "gs.h"

struct Native;

/* The first member of an exact backend's state (g->backend) that hosts the native renderer: the
   renderer finds itself there, and while it runs the exact backend clips every primitive to clip
   (inclusive pixels), so it draws only what the Z probe needs. */
typedef struct {
    struct Native *native;
    int clip_on, clip[4];
} NativeHost;

static inline struct Native *native_of(const Gs *g) { return ((const NativeHost *)g->backend)->native; }

#endif
