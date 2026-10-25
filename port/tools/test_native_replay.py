"""Native renderer pictures over local GS captures (D3D11 or offscreen Linux Vulkan).

Builds the replay with this tree's native renderer, runs every capture given and writes, per
capture, each field's picture hash and the readbacks' hash to OUT/summary.txt. With --expect, the
run fails unless the summary is the same as an earlier one: a refactor of the renderer that
changes no picture passes. Captures are game-derived and stay local (build/).

On Linux the Windows test runs under Wine (winegcc builds it against Wine's import libraries).
The Wine prefix (WINEPREFIX) needs a D3D11 with double-precision shader support (DXVK) and
Microsoft's d3dcompiler_47.dll: Wine's own compiler can't compile raster.hlsl (finding 081).

--tree builds another checkout's renderer (a git worktree of an older commit) with this replay,
to record the summary a change is compared with; --env runs with a renderer option set
(DHD_INTERP=1, DHD_SCALE=1...), which the comparison must use on both sides.

usage: python port/tools/test_native_replay.py OUT capture.gs... [--expect OLD/summary.txt]
       [--tree CHECKOUT] [--env NAME=VALUE]..."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ('test_native_replay.c', 'gpu.c', 'native.c', 'gs.c', 'memory.c')
LINUX = sys.platform.startswith('linux')


def build(out, tree, vulkan=False, source='test_native_replay.c'):
    if vulkan:
        from vulkan_shaders import generate
        generate(out / "gs_spirv.h")
        sources = [ROOT / "port/gs" / source,
                   *[tree / "port/gs" / name for name in ("native.c", "gs.c", "memory.c")],
                   *[tree / "port/platform/linux" / name for name in ("gpu.c", "native_gpu.c", "win32.c")]]
        import sdl3
        sdl_lib, sdl_include = sdl3.build()
        exe = out / "test-native-vulkan"
        subprocess.run(["clang", "-m32", "-O2", "-msse2", "-ffp-contract=off",
                        "-DDHD_NATIVE_VULKAN", "-I", str(sdl_include), "-I", str(out), "-I", str(tree / "port/include"),
                        "-I", str(tree / "port/platform/linux/include"),
                        *map(str, sources), "-o", str(exe), "-lvulkan", str(sdl_lib), *sdl3.LINUX_SYSTEM_LIBS], check=True)
        return exe
    shader = (tree / 'port/gs/raster.hlsl').read_text()
    (out / 'gs_shader.h').write_text(
        'static const char gs_shader[] = ' + json.dumps(shader) + ';\n'
        'static const char gs_shader_path[] = "port/gs/raster.hlsl";\n')
    exe = out / 'test-native-replay.exe'
    # The replay is this checkout's; the renderer the tree's (native.c only once it exists).
    sources = [str(ROOT / 'port/gs' / SOURCES[0])]
    sources += [str(tree / 'port/gs' / name) for name in SOURCES[1:] if (tree / 'port/gs' / name).exists()]
    flags = ['-O2', '-msse2', '-mfpmath=sse', '-ffp-contract=off', '-w', '-I', str(out), '-I', str(tree / 'port/gs')]
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
    ap.add_argument('--vulkan', action='store_true', help='build/run the Linux offscreen Vulkan backend')
    ap.add_argument('--validation', action='store_true', help='require Khronos validation and synchronization checks (--vulkan)')
    ap.add_argument('--all-pictures', action='store_true', help='save every field as PPM for cross-backend comparison')
    ap.add_argument('--expect', type=Path, help='an earlier summary.txt the pictures must match')
    ap.add_argument('--tree', type=Path, default=ROOT, help='the checkout whose renderer is built (default: this one)')
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE for the replay (DHD_* are otherwise cleared)')
    args = ap.parse_args()
    if args.validation and not args.vulkan:
        ap.error('--validation requires --vulkan')
    out = args.out.resolve()
    if ROOT / 'build' not in out.parents:
        ap.error('output must stay below build/')
    out.mkdir(parents=True, exist_ok=True)
    exe = build(out, args.tree.resolve(), args.vulkan)
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    env.setdefault('DXVK_STATE_CACHE_PATH', str(out))
    env.update(item.split('=', 1) for item in args.env)
    if args.all_pictures:
        env['DHD_REPLAY_ALL'] = '1'
    if args.validation:
        env['VK_LAYER_VALIDATE_SYNC'] = '1'
        env['DHD_VK_VALIDATE'] = '1'
    runner = ['wine', str(exe)] if LINUX and not args.vulkan else [str(exe)]
    if LINUX:
        env.setdefault('WINEDEBUG', '-all')
    lines = []
    for capture in args.captures:
        capture = capture.resolve()
        name = capture.relative_to(ROOT).as_posix() if ROOT in capture.parents else capture.name
        prefix = out / name.replace('/', '_').removesuffix('.gs')
        run = subprocess.run(runner + [str(capture), str(prefix)], cwd=ROOT, env=env, capture_output=True, text=True, timeout=180)
        lines.append('== ' + name)
        lines += [line for line in run.stdout.splitlines() if line.startswith(('field ', 'readbacks '))]
        lines += [line for line in run.stderr.splitlines() if line.startswith('profile native:')]
        (prefix.with_suffix('.stderr.txt')).write_text(run.stderr)
        (prefix.with_suffix('.stdout.txt')).write_text(run.stdout)
        if run.returncode:
            sys.stderr.write(run.stderr)
            return run.returncode
        if args.vulkan and any(marker in run.stdout + run.stderr for marker in
                               ('Validation Error', 'Validation Warning', 'VUID-', 'Vulkan validation:')):
            sys.stderr.write(run.stdout + run.stderr)
            return 1
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
