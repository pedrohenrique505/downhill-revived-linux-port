/* Host layout shared by D3D11 and Vulkan; raster.hlsl is the shader contract. */
#ifndef DHD_GS_RASTER_TYPES_H
#define DHD_GS_RASTER_TYPES_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
    uint32_t reg[128][4], position[3][4], uv[3][4];
    float stq[3][4];
    uint32_t bounds[4], info[4], palette[256];
} Constants;

enum { MAX_DRAWS = 256, MAX_BIN_WORDS = 1024 * 1024 };
typedef struct {
    uint32_t position[3][4], uv[3][4];
    float stq[3][4];
    uint32_t info[4], bounds[4];
    uint32_t depth[8];
} Primitive;

_Static_assert(sizeof(Constants) == 3248, "HLSL State layout");
_Static_assert(sizeof(Primitive) == 208, "HLSL Primitive stride");
_Static_assert(offsetof(Primitive, depth) == 176, "HLSL double plane layout");
#endif
