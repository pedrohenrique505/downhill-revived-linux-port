"""Synthetic Linux SDL/Vulkan window, movie, resize, close and 120/144 Hz interpolation.

No game data needed. Run on a desktop (or an X server); --validation requires
VK_LAYER_KHRONOS_validation, including its i686 shared library.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

from test_native_replay import build, ROOT


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--validation', action='store_true')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/port/issue16/window')
    args = ap.parse_args()
    out = args.out.resolve()
    if ROOT / 'build' not in out.parents:
        ap.error('output must stay below build/')
    out.mkdir(parents=True, exist_ok=True)
    exe = build(out, ROOT, True, 'test_native_window.c')
    base = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    if args.validation:
        base.update(DHD_VK_VALIDATE='1', VK_LAYER_VALIDATE_SYNC='1')
    for hz in (60, 120, 144, 0):
        env = dict(base, DHD_SCALE='window')
        if hz != 60:
            env['DHD_INTERP'] = '1'
            if hz:
                env['DHD_INTERP_HZ'] = str(hz)
        run = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=45)
        log = run.stdout + run.stderr
        (out / f'{hz}.log').write_text(log)
        print(run.stdout, end='')
        if run.returncode or 'PASS' not in run.stdout or any(m in log for m in
                ('Validation Error', 'Validation Warning', 'VUID-', 'Vulkan validation:')):
            sys.stderr.write(run.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
