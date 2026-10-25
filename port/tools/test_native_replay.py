"""Native renderer pictures over local GS captures (D3D11; port/gs/test_native_replay.c).

Builds the replay with this tree's native renderer, runs every capture given and writes, per
capture, each field's picture hash and the readbacks' hash to OUT/summary.txt. With --expect, the
run fails unless the summary is the same as an earlier one: a refactor of the renderer that
changes no picture passes. Captures are game-derived and stay local (build/).

On Linux the Windows test runs under Wine (winegcc builds it against Wine's import libraries).
The Wine prefix (WINEPREFIX) needs a D3D11 with double-precision shader support (DXVK) and
Microsoft's d3dcompiler_47.dll: Wine's own compiler can't compile raster.hlsl (finding 081).

usage: python port/tools/test_native_replay.py OUT capture.gs... [--expect OLD/summary.txt]"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ('test_native_replay.c', 'gpu.c', 'gs.c', 'memory.c')
LINUX = sys.platform.startswith('linux')


def build(out):
    shader = (ROOT / 'port/gs/raster.hlsl').read_text()
    (out / 'gs_shader.h').write_text(
        'static const char gs_shader[] = ' + json.dumps(shader) + ';\n'
        'static const char gs_shader_path[] = "port/gs/raster.hlsl";\n')
    exe = out / 'test-native-replay.exe'
    sources = [str(ROOT / 'port/gs' / name) for name in SOURCES]
    flags = ['-O2', '-msse2', '-mfpmath=sse', '-ffp-contract=off', '-w', '-I', str(out), '-I', str(ROOT / 'port/gs')]
    if LINUX:
        command = ['winegcc', '-m32', '-b', 'i686-windows', *flags, *sources, '-o', str(exe)]
    else:
        clang = shutil.which('clang') or str(Path.home() / 'scoop/apps/llvm/current/bin/clang.exe')
        command = [clang, '--target=i686-pc-windows-msvc', *flags, *sources, '-o', str(exe)]
    subprocess.run(command + ['-ld3d11', '-ldxgi', '-ld3dcompiler', '-luser32'], check=True)
    return exe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('captures', type=Path, nargs='+')
    ap.add_argument('--expect', type=Path, help='an earlier summary.txt the pictures must match')
    args = ap.parse_args()
    out = args.out.resolve()
    if ROOT / 'build' not in out.parents:
        ap.error('output must stay below build/')
    out.mkdir(parents=True, exist_ok=True)
    exe = build(out)
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    runner = ['wine', str(exe)] if LINUX else [str(exe)]
    if LINUX:
        env.setdefault('WINEDEBUG', '-all')
    lines = []
    for capture in args.captures:
        capture = capture.resolve()
        name = capture.relative_to(ROOT).as_posix() if ROOT in capture.parents else capture.name
        prefix = out / name.replace('/', '_').removesuffix('.gs')
        run = subprocess.run(runner + [str(capture), str(prefix)], cwd=ROOT, env=env, capture_output=True, text=True)
        lines.append('== ' + name)
        lines += [line for line in run.stdout.splitlines() if line.startswith(('field ', 'readbacks '))]
        if run.returncode:
            lines.append(f'exit {run.returncode}')
            sys.stderr.write(run.stderr)
    summary = '\n'.join(lines) + '\n'
    (out / 'summary.txt').write_text(summary)
    print(f'{sum(line.startswith("field ") for line in lines)} pictures from {len(args.captures)} captures: {out / "summary.txt"}')
    if args.expect:
        expected = args.expect.read_text()
        if expected != summary:
            old, new = expected.splitlines(), summary.splitlines()
            for a, b in zip(old, new):
                if a != b:
                    print(f'first difference: {a!r} -> {b!r}')
                    break
            print('DIFFERENT from', args.expect)
            return 1
        print('same pictures and readbacks as', args.expect)
    return 0


if __name__ == '__main__':
    sys.exit(main())
