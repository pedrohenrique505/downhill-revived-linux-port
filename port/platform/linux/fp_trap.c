/* port/runtime/fp_trap.c on Linux. The SSE trap (DHD_FP_TRAP=1) reads Windows' exception context and
   the lld-link map, so it is not available here yet; ps2_x87_checkpoint is the same as there. */
#include "fp_trap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ps2_fp_trap_init(void)
{
    const char *env = getenv("DHD_FP_TRAP");
    if (env && !strcmp(env, "1")) fprintf(stderr, "FP trap: not available on Linux yet\n");
}

void ps2_fp_trap_enable(void) {}

void ps2_x87_checkpoint(unsigned site, unsigned after)
{
    static int diagnostic = -1;
    if (diagnostic < 0) {
        const char *value = getenv("DHD_X87_CHECK");
        diagnostic = value && !strcmp(value, "1");
        if (diagnostic) fprintf(stderr, "x87 check: enabled, status and tags preserved\n");
    }
    if (!diagnostic) return;
    static unsigned frame, samples;
    if (site == 0x001B4128 && !after) ++frame;
    ++samples;
    unsigned env[7];
    __asm__ volatile("fnstenv %0\n\tfldenv %0" : "=m"(env) : : "memory");
    unsigned sw = env[1] & 0xffff, tags = env[2] & 0xffff;
    if (!(sw & 0x3841) && tags == 0xffff) {
        if (site == 0x001B4128 && !after && !(frame % 300))
            fprintf(stderr, "x87 check: frame=%u samples=%u SW=%04X TAG=%04X clean\n",
                    frame, samples, sw, tags);
        return;
    }
    static unsigned last_frame = ~0u, last_state, last_tags;
    unsigned state = sw & 0x3841;
    if (frame == last_frame && state == last_state && tags == last_tags) return;
    last_frame = frame; last_state = state; last_tags = tags;
    unsigned saved[27];
    __asm__ volatile("fnsave %0" : "=m"(saved) : : "memory");
    fprintf(stderr, "x87 fault: frame=%u site=%08X %s SW=%04X TOP=%u TAG=%04X FIP=%08X\n",
            frame, site, after ? "after" : "before",
            sw, (sw >> 11) & 7, tags, env[3]);
    fflush(stderr);
    __asm__ volatile("frstor %0" : : "m"(saved) : "memory");
}
