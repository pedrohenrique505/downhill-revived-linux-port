"""The PC target, shared by port/tools/build.py, data.py and link.py.

32-bit Windows with the Itanium C++ ABI (clang's windows-itanium): structs and bitfields are laid
out as by gcc, as on the PS2 (the MSVC ABI packs bitfields differently and 149 units use them),
and C++ member functions use the normal C calling convention, not thiscall, so C callers and the
game's function tables can call them. Objects are COFF and link with lld-link against the MSVC
C runtime.

Win32 C symbols carry a leading underscore (func_00123456 is _func_00123456 in the objects);
asm labels and the converted data add it by hand (LABEL_PREFIX).

On Linux (DHD_PLATFORM=linux, the default there) the objects are 32-bit x86 ELF: the game in
windows-itanium's ABI (below), the host code in the system's. ELF C symbols carry no prefix."""
import os
import sys

PLATFORM = os.environ.get('DHD_PLATFORM', 'linux' if sys.platform.startswith('linux') else 'windows')

if PLATFORM == 'linux':
    # The game keeps Windows' C ABI, in ELF objects: the decompiled source relies on it. A struct
    # returned through a hidden pointer is popped by the caller there (System V i386 pops it in the
    # callee, ret $4), so a unit that declares void f(Vec *out, int) and one that defines
    # Vec f(int) agree, as on the PS2; small structs come back in registers; the stack is only
    # 4-aligned and functions that need more realign it.
    TARGET = 'i686-pc-windows-itanium-elf'
    # The host code (port/hle, port/runtime, port/platform/linux) uses the system's headers and ABI.
    HOST_TARGET = 'i686-pc-linux-gnu'
    LABEL_PREFIX = ''
else:
    TARGET = 'i686-pc-windows-itanium'
    # The asm runtime and other host code use the platform's headers: plain C, same C ABI.
    HOST_TARGET = 'i686-pc-windows-msvc'
    LABEL_PREFIX = '_'

# The emulated PS2 RAM (32 MB) sits at this host address, with the uncached mirrors at
# 0x20000000/0x30000000 + it (port/runtime/memory.c). Below 0x10000000, so a PS2 DMA address
# (pointer & 0x0FFFFFFF) is still the host pointer; the exe is linked at IMAGE_BASE, above it all.
PS2_RAM_BASE = 0x08000000
IMAGE_BASE = 0x40000000


def asm_name(name):
    """The object symbol of a PS2 label: name with the C prefix. A .L label (a jump-table target)
    is an assembler-local name in ELF, so on Linux it becomes PS2L... (.L001FE0F4: PS2L001FE0F4)."""
    if PLATFORM == 'linux' and name.startswith('.L'):
        return 'PS2L' + name[2:]
    return LABEL_PREFIX + name
