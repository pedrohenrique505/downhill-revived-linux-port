"""Convert a completed-transfer PCSX2 v9 freeze to the port's legacy v8 dump.

Only the envelope and completed transfer bookkeeping change; registers, VRAM,
GIF path state and every packet remain verbatim. Incomplete transfers are
refused because the legacy format cannot represent their buffered bytes.
See PCSX2 v2.8.2 GSState::Freeze/GetSaveStateSize and GSDumpBase::AddHeader.
No upstream implementation is copied. Outputs must remain under ignored build/.
"""
import argparse
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]


def normalize(data):
    if len(data) < 44:
        raise ValueError('truncated modern GS dump header')
    marker, header_size = struct.unpack_from('<II', data)
    version, size, _, _, crc = struct.unpack_from('<5I', data, 8)
    if marker != 0xFFFFFFFF or header_size < 36 or version != 9 or size != 4194813:
        raise ValueError('expected PCSX2 v9 envelope and freeze')
    begin = 8 + header_size
    end = begin + size
    if len(data) < end + 8192 or struct.unpack_from('<I', data, begin)[0] != 9:
        raise ValueError('truncated or incompatible GS freeze')
    # v9 adds 61 bytes after transfer x/y, before VRAM. Its read/write buffer
    # is not serialized; require the transfer to have consumed all its bytes.
    total, start, consumed, writing = struct.unpack_from('<III?', data, begin + 364 + 48)
    if consumed != total or (writing and start != consumed):
        raise ValueError('incomplete GS transfer cannot be represented by v8')
    freeze = struct.pack('<I', 8) + data[begin + 4:begin + 364] + data[begin + 425:end]
    assert len(freeze) == 4194752
    return struct.pack('<II', crc, len(freeze)) + freeze + data[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / 'build'):
        parser.error('outputs must stay under build/')
    converted = normalize(args.source.read_bytes())
    with output.open('xb') as file:
        file.write(converted)
    print(f'Converted completed v9 transfer: {args.source} -> {output}')


if __name__ == '__main__':
    main()
