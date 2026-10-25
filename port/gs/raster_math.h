/* Shared exact-renderer host calculations; no backend handles or submissions. */
#ifndef DHD_GS_RASTER_MATH_H
#define DHD_GS_RASTER_MATH_H
#include "gs.h"
#include <stdlib.h>
#include <string.h>
static void gs_raster_texture_pages(uint32_t pages[16], Gs *g, unsigned ctxt)
{
    uint64_t tex = g->reg[GS_TEX0_1 + ctxt], clamp = g->reg[GS_CLAMP_1 + ctxt];
    uint64_t tex1 = g->reg[GS_TEX1_1 + ctxt];
    uint64_t mip[2] = {g->reg[GS_MIPTBP1_1 + ctxt], g->reg[GS_MIPTBP2_1 + ctxt]};
    unsigned mmin = (unsigned)(tex1 >> 6) & 7, levels = (unsigned)(tex1 >> 2) & 7;
    if (levels > 6) levels = 6;
    if (mmin < 2 || mmin > 5) levels = 0;
    memset(pages, 0, 16 * sizeof *pages);
    for (unsigned level = 0; level <= levels; level++) {
        unsigned lo[2] = {0,0}, hi[2];
        for (unsigned axis = 0; axis < 2; axis++) {
            unsigned mode = (unsigned)(clamp >> (axis * 2)) & 3;
            unsigned minimum = ((unsigned)(clamp >> (4 + axis * 20)) & 1023) >> level;
            unsigned maximum = ((unsigned)(clamp >> (14 + axis * 20)) & 1023) >> level;
            unsigned size = (unsigned)(tex >> (26 + axis * 4)) & 15;
            size = size > level ? size - level : 0;
            hi[axis] = size >= 11 ? 2047 : (1u << size) - 1;
            if (mode == 2) { lo[axis] = minimum; hi[axis] = maximum; }
            if (mode == 3) { lo[axis] = maximum; hi[axis] = maximum | minimum; }
            if (lo[axis] > hi[axis]) { lo[axis] = 0; hi[axis] = 2047; }
        }
        uint64_t location = level ? mip[(level - 1) / 3] >> (((level - 1) % 3) * 20) : tex;
        gs_mark_pages(pages, (unsigned)(tex >> 20) & 63, (unsigned)location & 16383,
                      (unsigned)(location >> 14) & 63, lo[0], lo[1], hi[0], hi[1]);
    }
}

static void gs_raster_depth(const GsVertex *v, unsigned type, unsigned count, uint32_t output[8])
{
    /* Keep integer Z exact through plane setup and interpolation. GS software
       rendering uses double precision for depth, independently of ST/RGBA. */
    double depth[4] = {v[0].z, 0, 0, 0};
    if (type == 1 || type == 2) {
        int dx = (int)v[1].x - (int)v[0].x, dy = (int)v[1].y - (int)v[0].y;
        unsigned axis = abs(dx) >= abs(dy) ? 1 : 2;
        int distance = axis == 1 ? dx : dy;
        if (distance) depth[axis] = ((double)v[1].z - v[0].z) / distance;
    } else if (count == 3) {
        double dx1 = (int)v[1].x - (int)v[0].x, dy1 = (int)v[1].y - (int)v[0].y;
        double dx2 = (int)v[2].x - (int)v[0].x, dy2 = (int)v[2].y - (int)v[0].y;
        double dz1 = (double)v[1].z - v[0].z, dz2 = (double)v[2].z - v[0].z;
        double area = dx1 * dy2 - dy1 * dx2;
        if (area) { depth[1] = (dz1 * dy2 - dy1 * dz2) / area; depth[2] = (dx1 * dz2 - dz1 * dx2) / area; }
    }
    memcpy(output, depth, sizeof depth);
}

static int gs_raster_independent_pixels(Gs *g, unsigned ctxt)
{
    uint64_t frame = g->reg[GS_FRAME_1 + ctxt], z = g->reg[GS_ZBUF_1 + ctxt];
    uint64_t scissor = g->reg[GS_SCISSOR_1 + ctxt];
    unsigned bw = (unsigned)(frame >> 16) & 63;
    unsigned x = (unsigned)(scissor >> 16) & 2047, y = (unsigned)(scissor >> 48) & 2047;
    /* Aliased rows or overlapping color/Z surfaces can communicate across
       pixels. Keep a dispatch boundary between such primitives. */
    if (!bw || x >= bw * 64 || (y / 32 + 1) * bw > 512) return 0;
    uint64_t test = g->reg[GS_TEST_1 + ctxt];
    if ((z >> 32 & 1) && (!(test >> 16 & 1) || (test >> 17 & 3) <= 1)) return 1;
    uint32_t color[16] = {0}, depth[16] = {0};
    gs_mark_pages(color, (unsigned)(frame >> 24) & 63, ((unsigned)frame & 511)*32, bw, 0,0,x,y);
    gs_mark_pages(depth, 48 | ((unsigned)(z >> 24) & 15), ((unsigned)z & 511)*32, bw, 0,0,x,y);
    for (unsigned i = 0; i < 16; i++) if (color[i] & depth[i]) return 0;
    return 1;
}

#endif
