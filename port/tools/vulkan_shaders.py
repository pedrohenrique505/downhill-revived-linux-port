"""Compile the shared exact rasterizer with official DXC, embedding SPIR-V in the host build.

DXC is Microsoft's release build, pinned: downloaded once into build/downloads (SHA-256 checked,
nothing else is accepted, as sdl3.py does with SDL) and unpacked under build/port/dxc. The
exactness tests were run with this version. DXC=path uses another dxc instead; without network
and without a download, a dxc on PATH is used, with a warning. DXC is MIT and LLVM licensed
(the licences are in the archive)."""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
DOWNLOADS = ROOT / 'build' / 'downloads'
OUT = ROOT / 'build' / 'port' / 'dxc'

VERSION = '1.9.2609'
ARCHIVE = 'linux_dxc_2026_09_28.x86_x64.tar.gz'
URL = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v%s/%s' % (VERSION, ARCHIVE)
SHA256 = '96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1'


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def download():
    """The pinned archive in build/downloads, fetched if it isn't there yet."""
    archive = DOWNLOADS / ARCHIVE
    if archive.exists() and sha256(archive) == SHA256:
        return archive
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    partial = archive.with_suffix('.part')
    print('DXC: downloading %s' % URL)
    with urllib.request.urlopen(URL, timeout=120) as response, open(partial, 'wb') as f:
        shutil.copyfileobj(response, f)
    if sha256(partial) != SHA256:
        partial.unlink()
        raise RuntimeError('DXC: %s does not have the pinned SHA-256' % URL)
    partial.replace(archive)
    return archive


def compiler():
    """The dxc to run: DXC if set, else the pinned release (unpacked on first use)."""
    if os.environ.get('DXC'):
        found = shutil.which(os.environ['DXC'])
        if not found:
            raise RuntimeError('DXC=%s is not an executable' % os.environ['DXC'])
        return found
    dxc = OUT / 'bin' / 'dxc'
    stamp = OUT / 'archive.sha256'
    if dxc.exists() and stamp.exists() and stamp.read_text().strip() == SHA256:
        return str(dxc)
    try:
        archive = download()
    except (OSError, RuntimeError) as e:
        found = shutil.which('dxc')
        if not found:
            raise RuntimeError('DXC: no pinned release (%s) and no dxc on PATH; set DXC to one' % e)
        print('DXC: warning: %s; using %s, not the pinned %s' % (e, found, VERSION), file=sys.stderr)
        return found
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    with tarfile.open(archive) as tar:
        tar.extractall(OUT, filter='data')
    stamp.write_text(SHA256 + '\n')
    return str(dxc)


def generate(output):
    output = Path(output)
    source = ROOT / 'port/gs/raster.hlsl'
    compiler_path = compiler()
    arrays = []
    # main draws; local_copy is TRXDIR 2. Its Copy cbuffer (b2) moves past t0..t3.
    for entry, name, extra in (('main', 'gs_spirv', []), ('local_copy', 'gs_spirv_copy', ['-fvk-b-shift', '8', '0'])):
        binary = output.with_name(output.stem + '_' + entry + '.spv')
        subprocess.run([compiler_path, '-spirv', '-T', 'cs_6_0', '-E', entry,
                        '-Gis', '-D', 'DHD_EARLY_DEPTH=1', '-D', 'DHD_EXCLUSIVE_WORD_STORE=1', '-D', 'DHD_STATE_TABLE=1',
                        '-fspv-target-env=vulkan1.2', '-fvk-use-dx-layout', *extra,
                        '-fvk-u-shift', '1', '0', '-fvk-t-shift', '2', '0',
                        '-Fo', str(binary), str(source)], check=True)
        code = binary.read_bytes()
        words = struct.unpack('<%dI' % (len(code) // 4), code)
        arrays.append('static const uint32_t %s[] = {\n' % name +
                      ',\n'.join(','.join('0x%08xu' % w for w in words[i:i+8])
                                 for i in range(0, len(words), 8)) + '\n};\n')
    output.write_text('/* Generated from port/gs/raster.hlsl; do not edit. */\n' + ''.join(arrays))


if __name__ == '__main__':
    import sys
    generate(sys.argv[1])
