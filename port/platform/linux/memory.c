/* The PS2 address space on Linux: port/runtime/memory.c's layout (read its comment first), with
   POSIX calls. The 32 MB of PS2 RAM are one memfd, mapped shared at PS2_RAM_BASE and at
   0x20000000/0x30000000 + PS2_RAM_BASE, and from PS2_LOW_VIEW up also at the PS2 addresses
   themselves (and their mirrors). The executable is linked at 0x40000000 and the system puts
   shared libraries near the top of the 32-bit space, so these addresses are free at start and
   no relaunch is needed to reserve them (Windows' host_relaunch_reserving). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "memory.h"
#include "ps2asm.h"

#include <errno.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* The words of the image the PC build changes (port/tools/data.py fixups.s), as on Windows. */
extern const uint32_t ps2_fixups[][2] __asm__("_ps2_fixups");
extern const uint32_t ps2_fixups_end[][2] __asm__("_ps2_fixups_end");

/* Data words that hold a fixed PS2 RAM address as a plain number (see port/runtime/memory.c). */
static const uint32_t ram_address_words[] = {0x29D9D4, 0x29DB0C, 0x29DB10, 0x29DB14, 0x29DB18, 0x29DB1C};

static int map_view(int fd, uintptr_t at, off_t offset, size_t size)
{
    void *p = mmap((void *)at, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, offset);
    if (p == (void *)at) return 1;
    fprintf(stderr, "ps2_memory_init: can't map PS2 RAM at %08lX (%s)\n", (unsigned long)at,
            p == MAP_FAILED ? strerror(errno) : "placed elsewhere");
    if (p != MAP_FAILED) munmap(p, size);
    return 0;
}

static int alloc_at(uintptr_t at, size_t size)
{
    void *p = mmap((void *)at, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == (void *)at) return 1;
    fprintf(stderr, "ps2_memory_init: %08lX is taken (%s)\n", (unsigned long)at,
            p == MAP_FAILED ? strerror(errno) : "placed elsewhere");
    if (p != MAP_FAILED) munmap(p, size);
    return 0;
}

int ps2_low_view_reserved(void) { return 1; }

/* The game writes some of its .rodata (func_00208900 uppercases a const bank name in place): the
   PS2 never protected it. Windows links .rdata writable; here the executable's read-only segment
   (lld puts .rodata in a PT_LOAD of its own, neither writable nor executable) is made writable. */
static int rodata_writable(struct dl_phdr_info *info, size_t size, void *unused)
{
    (void)size, (void)unused;
    if (info->dlpi_name && info->dlpi_name[0]) return 0;  /* the executable comes first, unnamed */
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || (ph->p_flags & (PF_W | PF_X))) continue;
        uintptr_t start = (info->dlpi_addr + ph->p_vaddr) & ~(uintptr_t)4095;
        uintptr_t end = (info->dlpi_addr + ph->p_vaddr + ph->p_memsz + 4095) & ~(uintptr_t)4095;
        if (mprotect((void *)start, end - start, PROT_READ | PROT_WRITE))
            fprintf(stderr, "ps2_memory_init: can't make %08lX-%08lX writable (%s)\n", (unsigned long)start,
                    (unsigned long)end, strerror(errno));
    }
    return 1;
}

int ps2_memory_init(void)
{
    dl_iterate_phdr(rodata_writable, NULL);
    int fd = memfd_create("ps2-ram", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, PS2_RAM_SIZE)) {
        fprintf(stderr, "ps2_memory_init: can't make the PS2 RAM (%s)\n", strerror(errno));
        return 0;
    }
    static const uint32_t mirrors[3] = {0, 0x20000000, 0x30000000};
    for (unsigned m = 0; m < 3; m++)
        if (!map_view(fd, mirrors[m] | PS2_RAM_BASE, 0, PS2_RAM_SIZE)
            || !map_view(fd, mirrors[m] | PS2_LOW_VIEW, PS2_LOW_VIEW, PS2_RAM_SIZE - PS2_LOW_VIEW))
            return 0;
    close(fd); /* the mappings keep it */
    if (!alloc_at(PS2_SCRATCHPAD, 0x4000) || !alloc_at(0x10000000, 0x10000) || !alloc_at(0x12000000, 0x2000)
        || !alloc_at(0x11000000, 0x10000)) {
        fprintf(stderr, "ps2_memory_init: can't map the scratchpad, VU memory or the hardware registers\n");
        return 0;
    }
    ps2.vu0_code = (uint8_t *)(uintptr_t)0x11000000;
    ps2.vu0_mem = (ps2_q128 *)(uintptr_t)0x11004000;
    return 1;
}

void ps2_image_load(const uint8_t *retail)
{
    memcpy(PS2_PTR(PS2_IMAGE_START), retail, PS2_IMAGE_END - PS2_IMAGE_START);
    for (const uint32_t (*f)[2] = ps2_fixups; f < ps2_fixups_end; f++)
        *(uint32_t *)PS2_PTR((*f)[0]) = (*f)[1];
    for (size_t i = 0; i < sizeof ram_address_words / sizeof *ram_address_words; i++)
        *(uint32_t *)PS2_PTR(ram_address_words[i]) += PS2_RAM_BASE;
    ps2_vu_aot_load(PS2_PTR(PS2_IMAGE_START));
}
