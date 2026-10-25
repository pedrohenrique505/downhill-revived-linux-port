"""Measure offscreen native replay pictures from two backends (no game data in the repo).

Generate both directories with test_native_replay.py --all-pictures, using identical
captures/options. Reports every RGB byte difference, and requires equal FIFO readbacks.
The defaults allow small hardware rasterization differences: RGB MAE <= 0.1 and
at most 0.1% changed pixels per field. No pixels are excluded or shifted.
Usage: python port/tools/compare_native_pictures.py D3D_DIR VULKAN_DIR REPORT.json
"""
import argparse
import json
from pathlib import Path


def summary(directory):
    result = {}
    for block in (directory / 'summary.txt').read_text().split('== ')[1:]:
        lines = block.splitlines()
        result[lines[0]] = {
            'fields': [line for line in lines if line.startswith('field ')],
            'reads': [line for line in lines if line.startswith('readbacks ')],
        }
    return result


def ppm(path):
    magic, size, maximum, pixels = path.read_bytes().split(b'\n', 3)
    w, h = map(int, size.split())
    if magic != b'P6' or maximum != b'255' or len(pixels) != w*h*3:
        raise ValueError(f'invalid PPM: {path}')
    return w, h, pixels


def compare(reference, candidate, max_mae=0.1, max_fraction=0.001):
    old, new = summary(reference), summary(candidate)
    if old.keys() != new.keys():
        raise ValueError('capture lists differ')
    pictures = []
    fields = same_hash = 0
    for name, current in new.items():
        before = old[name]
        if before['reads'] != current['reads']:
            raise ValueError(f'FIFO readbacks differ: {name}')
        if len(before['fields']) != len(current['fields']):
            raise ValueError(f'field count differs: {name}')
        for a, b in zip(before['fields'], current['fields']):
            fields += 1
            same_hash += a == b
    old_files = {p.name for p in reference.glob('*.ppm')}
    new_files = {p.name for p in candidate.glob('*.ppm')}
    if old_files != new_files or len(new_files) != fields:
        raise ValueError('expected one matching PPM per field; use --all-pictures on both replays')
    for name in sorted(new_files):
        w, h, a = ppm(reference / name)
        x, y, b = ppm(candidate / name)
        if (w, h) != (x, y):
            raise ValueError(f'picture dimensions differ: {name}')
        changed = total = maximum = over16 = 0
        if a != b:
            for i in range(0, len(a), 3):
                delta = [abs(a[i+c]-b[i+c]) for c in range(3)]
                worst = max(delta)
                total += sum(delta)
                maximum = max(maximum, worst)
                changed += worst != 0
                over16 += worst > 16
        mae, fraction = total / len(a), changed / (w*h)
        pictures.append({'picture': name, 'width': w, 'height': h, 'rgb_mae': mae,
                         'max_channel_error': maximum, 'different_pixels': changed,
                         'pixels_over_16': over16, 'different_fraction': fraction,
                         'pass': mae <= max_mae and fraction <= max_fraction})
    return {'fields': fields, 'identical_rgba_hashes': same_hash, 'matching_readback_captures': len(new),
            'limits': {'rgb_mae': max_mae, 'different_fraction': max_fraction},
            'pass': all(p['pass'] for p in pictures), 'pictures': pictures}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('reference', type=Path)
    ap.add_argument('candidate', type=Path)
    ap.add_argument('report', type=Path)
    ap.add_argument('--max-mae', type=float, default=0.1)
    ap.add_argument('--max-different-fraction', type=float, default=0.001)
    args = ap.parse_args()
    report = compare(args.reference, args.candidate, args.max_mae, args.max_different_fraction)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f"{report['fields']} fields, {report['identical_rgba_hashes']} identical RGBA hashes; "
          f"{report['matching_readback_captures']} matching FIFO transcripts")
    for picture in report['pictures']:
        if picture['different_pixels']:
            print(f"{picture['picture']}: MAE {picture['rgb_mae']:.9f}, "
                  f"max {picture['max_channel_error']}, changed {picture['different_pixels']}, "
                  f"over16 {picture['pixels_over_16']}")
    print('PASS' if report['pass'] else 'FAIL', args.report)
    return 0 if report['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
