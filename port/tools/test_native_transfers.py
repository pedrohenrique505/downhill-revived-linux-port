"""Synthetic native-GS upload/copy ordering checks (D3D11 or Vulkan, no game data)."""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/port/test-native-transfers'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vulkan', action='store_true')
    parser.add_argument('--validation', action='store_true')
    args = parser.parse_args()
    if args.validation and not args.vulkan:
        parser.error('--validation requires --vulkan')
    OUT.mkdir(parents=True, exist_ok=True)
    if args.vulkan:
        from test_native_replay import build
        exe = build(OUT, ROOT, True, 'test_native_transfers.c')
        env = os.environ.copy()
        if args.validation:
            env['DHD_VK_VALIDATE'] = '1'
            env['VK_LAYER_VALIDATE_SYNC'] = '1'
        run = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=120)
        print(run.stdout, end='')
        sys.stderr.write(run.stderr)
        (OUT / 'vulkan.log').write_text(run.stdout + run.stderr)
        return run.returncode or int(any(m in run.stdout + run.stderr for m in
                                        ('Validation Error', 'Validation Warning', 'VUID-', 'Vulkan validation:')))

    shader = (ROOT / 'port/gs/raster.hlsl').read_text()
    (OUT / 'gs_shader.h').write_text(
        'static const char gs_shader[] = ' + json.dumps(shader) + ';\n'
        'static const char gs_shader_path[] = "port/gs/raster.hlsl";\n')
    clang = shutil.which('clang') or str(Path.home() / 'scoop/apps/llvm/current/bin/clang.exe')
    exe = OUT / 'test-native-transfers.exe'
    command = [clang, '--target=i686-pc-windows-msvc', '-O2', '-w',
               '-I', str(OUT), '-I', str(ROOT / 'port/gs')]
    command += [str(ROOT / 'port/gs' / name) for name in
                ('test_native_transfers.c', 'gpu.c', 'native.c', 'gs.c', 'memory.c')]
    command += ['-o', str(exe), '-ld3d11', '-ldxgi', '-ld3dcompiler', '-luser32']
    subprocess.run(command, check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    return subprocess.call([str(exe)], cwd=ROOT, env=env)


if __name__ == '__main__':
    sys.exit(main())
