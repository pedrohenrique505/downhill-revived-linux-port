/* MSVC intrinsics, for Linux: the x86 ones come from the compiler headers. */
#pragma once
#include <x86intrin.h>

/* The lowest set bit of mask in *index; 0 when there is none. */
static inline unsigned char _BitScanForward(unsigned long *index, unsigned long mask)
{
    if (!mask) return 0;
    *index = (unsigned long)__builtin_ctzl(mask);
    return 1;
}
