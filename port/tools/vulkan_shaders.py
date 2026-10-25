"""Compile the shared exact rasterizer with official DXC, embedding SPIR-V in the host build."""
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def generate(output):
    output = Path(output)
    source = ROOT / 'port/gs/raster.hlsl'
    compiler = shutil.which(os.environ.get('DXC', 'dxc'))
    if not compiler:
        raise RuntimeError('Linux exact renderer requires DXC; set DXC to the official dxc executable')
    arrays = []
    # main draws; local_copy is TRXDIR 2. Its Copy cbuffer (b2) moves past t0..t3.
    for entry, name, extra in (('main', 'gs_spirv', []), ('local_copy', 'gs_spirv_copy', ['-fvk-b-shift', '8', '0'])):
        binary = output.with_name(output.stem + '_' + entry + '.spv')
        subprocess.run([compiler, '-spirv', '-T', 'cs_6_0', '-E', entry,
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
