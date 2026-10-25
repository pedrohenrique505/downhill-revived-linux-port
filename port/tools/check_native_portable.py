"""The native renderer's frontend (port/gs/native.c) builds without a graphics API.

Compiles native.c as link.py does for this platform, with every Direct3D and DXGI header replaced
by one that stops the compile: the frontend may use only native.h's backend interface. On Linux
it also compiles with the port's <windows.h> and other host headers (port/platform/linux/include),
which the Vulkan backend will link it with.

usage: python port/tools/check_native_portable.py"""
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import link  # noqa: E402
from pctarget import HOST_TARGET  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/port/native-portable'
GRAPHICS = ('d3d11.h', 'd3d11_1.h', 'd3d11_2.h', 'd3d11_3.h', 'd3d11_4.h', 'd3dcommon.h', 'd3dcompiler.h',
            'dxgi.h', 'dxgi1_2.h', 'dxgi1_3.h', 'dxgi1_4.h', 'dxgi1_5.h', 'dxgi1_6.h', 'vulkan/vulkan.h')


def main():
    poison = OUT / 'poison'
    for name in GRAPHICS:
        path = poison / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f'#error "the native renderer\'s frontend includes <{name}>: only its backend may"\n')
    clang = shutil.which('clang') or str(Path.home() / 'scoop/apps/llvm/current/bin/clang.exe')
    command = [clang, '--target=' + HOST_TARGET, '-c', '-O2', '-msse2', '-mfpmath=sse', '-ffp-contract=off', '-Wall',
               '-I', str(poison), '-I', str(ROOT / 'port/gs'), '-I', str(ROOT / 'port/runtime')]
    if link.LINUX:
        command += link.LINUX_CFLAGS + ['-I', str(ROOT / 'port/hle')]
    command += [str(ROOT / 'port/gs/native.c'), '-o', str(OUT / 'native.o')]
    if subprocess.run(command).returncode:
        print('native.c does not build without a graphics API')
        return 1
    print(f'native.c builds for {HOST_TARGET} with no Direct3D, DXGI or Vulkan header')
    return 0


if __name__ == '__main__':
    sys.exit(main())
