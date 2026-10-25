"""Replay a player-owned GS capture through Linux exact, checking speed and full VRAM.

Build first with --build. Captures and outputs remain outside version control.
Example: python port/tools/bench_exact_linux.py capture.gs --max-ms 33.333
Use --reference previous.vram to require byte-identical final GS memory.
When previous.readbacks exists, compare recorded GS read responses as well.
Timing includes GIF processing, synchronization and CPU scanout, excludes startup,
shader compilation and picture-file output; it is not whole-game FPS.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
from vulkan_shaders import ROOT, generate


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--build', action='store_true')
    ap.add_argument('--exe', type=Path, default=ROOT / 'build/port/gs-replay/replay')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/port/gs-replay/latest')
    ap.add_argument('--reference', type=Path)
    ap.add_argument('--max-ms', type=float)
    args = ap.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    if args.build:
        args.exe.parent.mkdir(parents=True, exist_ok=True)
        generate(args.exe.parent / 'gs_spirv.h')
        sources = [ROOT / 'port/gs' / (n + '.c') for n in ('memory', 'gs', 'display', 'replay')]
        subprocess.run([os.environ.get('CC', 'clang'), '-m32', '-O2', '-msse2',
                        '-ffp-contract=off', '-Wall', '-Wextra', '-I', str(args.exe.parent),
                        *map(str, sources), str(ROOT / 'port/platform/linux/gpu.c'),
                        '-lvulkan', '-o', str(args.exe)], check=True)
    reference = args.reference.read_bytes() if args.reference else None
    read_reference = args.reference.with_suffix('.readbacks') if args.reference else None
    readbacks = read_reference.read_bytes() if read_reference and read_reference.exists() else None
    result = subprocess.run([str(args.exe.resolve()), str(args.capture.resolve()), str(args.out.resolve())],
                            env=dict(os.environ, DHD_REPLAY_NO_IMAGES='1'),
                            check=True, text=True, stdout=subprocess.PIPE, timeout=60)
    print(result.stdout, end='')
    match = re.search(r'ms_per_field=([\d.]+)', result.stdout)
    if not match:
        raise SystemExit('FAIL: missing replay timing')
    if args.reference:
        if args.out.with_suffix('.vram').read_bytes() != reference:
            raise SystemExit('FAIL: final VRAM differs')
        print('Full VRAM: MATCH')
        if readbacks is not None:
            if args.out.with_suffix('.readbacks').read_bytes() != readbacks:
                raise SystemExit('FAIL: GS readbacks differ')
            print('GS readbacks: MATCH')
    if args.max_ms is not None and float(match[1]) > args.max_ms:
        raise SystemExit(f'FAIL: exact raster exceeds {args.max_ms:.3f} ms/field')


if __name__ == '__main__':
    main()
