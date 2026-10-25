"""Build and run Linux exact-GS tests through public GIF/register/VRAM interfaces.

Needs a C compiler, Vulkan headers/loader and an i686 Vulkan driver; the pinned
DXC is fetched by vulkan_shaders.py (DXC=path selects another one).
The fixtures are shared with D3D11 WARP; they contain no game data.
LD_LIBRARY_PATH can select
an isolated i686 driver. Run from anywhere: python port/tools/test_vulkan.py.
"""
import argparse
import os
from pathlib import Path
import subprocess

from vulkan_shaders import ROOT, generate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-only', action='store_true')
    args = parser.parse_args()
    out = ROOT / 'build/port/test-vulkan'
    out.mkdir(parents=True, exist_ok=True)
    generate(out / 'gs_spirv.h')
    common = [os.environ.get('CC', 'clang'), '-m32', '-O2', '-msse2',
              '-ffp-contract=off', '-Wall', '-Wextra', '-I', str(out)]
    core = [str(ROOT / 'port/gs' / name) for name in ('gs.c', 'memory.c', 'display.c')]
    tests = {
        'gs': [str(ROOT / 'port/gs/test_gs.c'), *core],
        'vulkan': [str(ROOT / 'port/gs/test_gpu.c'), str(ROOT / 'port/platform/linux/gpu.c'), *core, '-lvulkan'],
    }
    for name, sources in tests.items():
        executable = out / name
        subprocess.run([*common, *sources, '-o', str(executable)], check=True)
        if not args.build_only:
            subprocess.run([str(executable)], check=True, timeout=120)


if __name__ == '__main__':
    main()
