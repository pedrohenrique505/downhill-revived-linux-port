"""Native renderer paths a GS capture replay doesn't reach, run in the game (dhd.exe, D3D11).

test_native_replay.py checks recording, the probe, textures and draws over GS captures, without a
window and with the texture pack and the sharp font stubbed. This runs the game itself, windowed
(the swap chain and its presentation), with DHD_RENDER=native and the virtual frame clock, and
hashes the pictures it saves (DHD_CAPTURE_FIELDS: the native renderer's own picture) in:
  boot      the startup logo movies (the movie picture through the native scanout);
  menu      the title and main menu in the sharp font (the default);
  font      the same with dhd.ini font = original (DHD_FONT), the game's own texture;
  texpack   the same menus with a texture pack (DHD_TEXTURE_PACK): every texture the font run's
            first menus drew, replaced by a 2x checker in a colour from its name.
The pack is made from a first, unpacked run's DHD_TEXTURE_PACK_LOG, or taken from --pack (an
earlier run's OUT/pack, so two builds draw the same one). Replacements load on threads and draw
once loaded, so the texpack pictures are taken late on screens that stay put.

OUT/summary.txt lists each picture's hash; --expect compares it with an earlier run (another
build: a refactor that changes no picture passes). Pictures and the pack are game-derived and stay
local (build/). On Linux dhd.exe runs under Wine: WINEPREFIX needs DXVK and d3dcompiler_47
(finding 081).

usage: DHD_ISO=... python port/tools/test_native_paths.py OUT --exe dhd.exe [--pack DIR]
       [--expect OLD/summary.txt]"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[2]
LINUX = sys.platform.startswith('linux')
COMMON = dict(DHD_RENDER='native', DHD_FRAME_CLOCK='1', DHD_SND_OFF='1', DHD_MEMCARD='0', DHD_SETTINGS='none',
              DHD_LOG='1')
MENUS = dict(DHD_SKIP_MOVIES='1', DHD_NAV='0:triangle|cross')
# fields: the two silent logo movies (Triangle on the memory card dialog; the movies with sound
# show the picture the sound mixer's clock is at, which moves with wall time); the title and main
# menu (movies skipped)
CASES = {
    'boot': (dict(DHD_NAV='0:triangle'), (150, 450)),
    'menu': (dict(MENUS), (600, 900, 1200)),
    'font': (dict(MENUS, DHD_FONT='original'), (600, 900, 1200)),
    'texpack': (dict(MENUS), (900, 1200)),
}


def command(exe):
    return ['wine', str(exe)] if LINUX else [str(exe)]


def bmp_complete(path):
    header = path.read_bytes()[:6] if path.exists() else b''
    return len(header) == 6 and header[:2] == b'BM' and int.from_bytes(header[2:6], 'little') == path.stat().st_size


def run_case(exe, directory, extra, fields, timeout):
    """Runs the game until it has saved every field's picture; {field: path}."""
    directory.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('DHD_', 'ROUND_'))}
    env.update(COMMON, DHD_ISO=os.environ['DHD_ISO'], DHD_SCREENSHOT=str(directory / 'f'),
               DHD_CAPTURE_FIELDS=','.join(f'{f}-{f}' for f in fields), **extra)
    if LINUX:
        env.setdefault('WINEDEBUG', '-all')
        env.setdefault('DXVK_LOG_LEVEL', 'none')
    pictures = {f: directory / f'f-f{f:06}.bmp' for f in fields}
    with (directory / 'run.log').open('wb') as log:
        process = subprocess.Popen(command(exe), cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + timeout
        try:
            while not all(bmp_complete(p) for p in pictures.values()):
                if process.poll() is not None:
                    raise RuntimeError(f'game exited {process.returncode}: {directory}')
                if time.monotonic() > deadline:
                    raise TimeoutError(f'{timeout} s: {directory}')
                time.sleep(.5)
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    return pictures


def png(path, width, height, rgba_rows):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    raw = b''.join(b'\0' + row for row in rgba_rows)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def make_pack(log, pack):
    """A replacement for every texture the log names: a 2x checker of a colour from its name, GS
    alpha 128 (opaque)."""
    replacements = pack / 'replacements'
    replacements.mkdir(parents=True)
    names = {}
    for line in log.read_text().splitlines()[1:]:
        field, psm, width, height, name, found, mips = line.split(',')
        names.setdefault(name, (int(width), int(height)))
    for name, (width, height) in sorted(names.items()):
        w, h = min(width * 2, 1024), min(height * 2, 1024)
        c = hashlib.sha256(name.encode()).digest()
        a, b = bytes((c[0], c[1], c[2], 128)), bytes((c[3], c[4], c[5], 128))
        rows = [b''.join(a if (x // 8 + y // 8) % 2 else b for x in range(w)) for y in range(h)]
        png(replacements / f'{name}.png', w, h, rows)
    return len(names)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--exe', type=Path, required=True)
    ap.add_argument('--pack', type=Path, help="an earlier run's OUT/pack (default: make one)")
    ap.add_argument('--expect', type=Path, help='an earlier summary.txt the pictures must match')
    ap.add_argument('--timeout', type=float, default=300, help='seconds per run')
    args = ap.parse_args()
    out = args.out.resolve()
    if ROOT / 'build' not in out.parents:
        ap.error('output must stay below build/')
    if not os.environ.get('DHD_ISO'):
        ap.error('DHD_ISO must reference the local game image')
    out.mkdir(parents=True, exist_ok=False)
    exe = out / 'frozen.exe'
    shutil.copy2(args.exe.resolve(), exe)
    lines = [f'exe {digest(exe)}']
    pictures = {}
    for name in ('boot', 'menu', 'font'):
        extra, fields = CASES[name]
        pictures[name] = run_case(exe, out / name, extra, fields, args.timeout)
    pack = out / 'pack'
    if args.pack:
        shutil.copytree(args.pack.resolve(), pack)
    else:
        empty = out / 'pack-log'
        (empty / 'replacements').mkdir(parents=True)
        # a pack with no file isn't opened: one whose name no texture has
        png(empty / 'replacements' / '0-00000000.png', 1, 1, [bytes(4)])
        log = empty / 'textures.csv'
        run_case(exe, empty / 'run', dict(CASES['texpack'][0], DHD_FONT='original', DHD_TEXTURE_PACK=str(empty),
                                          DHD_TEXTURE_PACK_LOG=str(log)), CASES['texpack'][1], args.timeout)
        print(f'pack: {make_pack(log, pack)} replacements')
    files = sorted((pack / 'replacements').glob('*.png'))
    lines.append(f'pack {len(files)} ' + hashlib.sha256(b''.join(f.name.encode() + bytes.fromhex(digest(f))
                                                                  for f in files)).hexdigest())
    extra, fields = CASES['texpack']
    log = out / 'texpack-textures.csv'
    pictures['texpack'] = run_case(exe, out / 'texpack', dict(extra, DHD_FONT='original', DHD_TEXTURE_PACK=str(pack),
                                                               DHD_TEXTURE_PACK_LOG=str(log)), fields, args.timeout)
    found = sum(line.split(',')[5] == '1' for line in log.read_text().splitlines()[1:])
    if not found:
        print('the texture pack replaced nothing:', log)
        return 1
    for name, shots in pictures.items():
        lines += [f'{name} field {f} {digest(p)}' for f, p in sorted(shots.items())]
    summary = '\n'.join(lines) + '\n'
    (out / 'summary.txt').write_text(summary)
    print(f'{sum(map(len, pictures.values()))} pictures, {found} replaced draws: {out / "summary.txt"}')
    if args.expect:
        old = [l for l in args.expect.read_text().splitlines() if not l.startswith('exe ')]
        new = [l for l in lines if not l.startswith('exe ')]
        if old != new:
            for line in sorted(set(old) ^ set(new)):
                print(('-' if line in old else '+') + ' ' + line)
            print('DIFFERENT from', args.expect)
            return 1
        print('same pictures as', args.expect)
    return 0


if __name__ == '__main__':
    sys.exit(main())
