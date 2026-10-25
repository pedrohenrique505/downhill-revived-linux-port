"""Check Linux exact GS replay against PCSX2 software rasterization.

Player-owned captures and all outputs must stay under ignored build/. This is
a GS-only check; it does not claim independent retail boot equivalence.
Uses compare_pcsx2's display-register crop, with no alignment search.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

import numpy as np
from PIL import Image

from compare_pcsx2 import ROOT, comparison, read_dump, scanout, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--pcsx2', type=Path, default=ROOT / 'build/port/compare-pcsx2-build/bin/pcsx2-gsrunner')
    parser.add_argument('--replay', type=Path, default=ROOT / 'build/port/gs-replay/replay')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--window', action='store_true', help='use X11 when surfaceless EGL is unavailable')
    args = parser.parse_args()
    args.capture = args.capture.resolve()
    args.out = args.out.resolve()
    if not args.out.is_relative_to(ROOT / 'build'):
        parser.error('outputs must stay under build/')
    states = read_dump(args.capture)
    if len(states) < 2:
        parser.error('need a freeze field and at least one independently replayed field')
    args.out.mkdir(parents=True, exist_ok=False)
    reference = args.out / 'reference'
    reference.mkdir()
    env = dict(os.environ)
    env.pop('DHD_REPLAY_NO_IMAGES', None)
    commands = [
        [str(args.replay.resolve()), str(args.capture), str(args.out / 'exact')],
        [str(args.pcsx2.resolve()), '-window' if args.window else '-surfaceless', '-renderer', 'sw', '-loop', '1',
         '-dump', 'f', '-dumprangef', f'0,{len(states)},1', '-dumpdir', str(reference),
         '--', str(args.capture)],
    ]
    for i, command in enumerate(commands):
        if i == 1:
            (args.out / 'reference.readbacks').touch()
            env['DHD_COMPARE_READBACKS'] = str(args.out / 'reference.readbacks')
        with (args.out / f'run-{i}.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                           env=env, check=True, timeout=60)
    files = {}
    for path in sorted(reference.iterdir()):
        match = re.search(r'_f(\d+)_fr([01])_', path.name)
        if match and path.suffix.lower() in ('.png', '.bmp'):
            files.setdefault((int(match[1]), int(match[2])), path)
    scores = []
    for field, state in enumerate(states[1:], 1):
        path = files.get((field, state['circuit']))
        if path is None or f'_{state["block"]:05x}_' not in path.name:
            raise RuntimeError(f'missing PCSX2 framebuffer for field {field}: {state}')
        a = scanout(path, state)
        b = Image.open(args.out / f'exact-{field:03d}.ppm').convert('RGB')
        class Options:
            threshold = 16
            line_shift = 0
        score = comparison(a, b, Options, args.out / f'comparison-{field:03d}.png')
        diff = np.abs(np.asarray(a, dtype=np.int16) - np.asarray(b, dtype=np.int16))
        ys, xs = np.nonzero(diff.max(axis=2) > 16)
        score.update(field=field, display=state, max_channel_error=int(diff.max()),
                     pixels_over_16=len(xs), outliers=[dict(x=int(x), y=int(y),
                     reference=list(map(int, np.asarray(a)[y, x])),
                     exact=list(map(int, np.asarray(b)[y, x]))) for x, y in zip(xs, ys)])
        scores.append(score)
        print(f'field={field} MAE={score["mae"]:.9f} max={score["max_channel_error"]} pixels_over_16={len(xs)}')
    result = dict(capture=str(args.capture), capture_sha256=sha256(args.capture),
                  pcsx2_sha256=sha256(args.pcsx2), replay_sha256=sha256(args.replay),
                  scope='GS replay, not independent retail boot', fields=scores)
    exact_reads = (args.out / 'exact.readbacks').read_bytes()
    reference_reads = (args.out / 'reference.readbacks').read_bytes()
    result['readbacks'] = dict(bytes=len(exact_reads), reference_bytes=len(reference_reads),
                               match=exact_reads == reference_reads)
    (args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if exact_reads != reference_reads:
        raise SystemExit('FAIL: GS readbacks differ; see result.json')
    print(f'GS readbacks: {len(exact_reads)} bytes, MATCH' if exact_reads else 'GS readbacks: none in this capture')
    if any(score['pixels_over_16'] for score in scores):
        raise SystemExit('FAIL: pixels exceed 16; investigate result.json')
    print('PASS: no pixel exceeds 16')


if __name__ == '__main__':
    main()
