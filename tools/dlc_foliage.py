"""Recompress terrain/*/foliagemap/foliage.raw cells from Xbox XMem to PC zlib.

Layout (same on both platforms apart from the cell compression): the first 65536 bytes are
16384 big-endian u32 cell start offsets relative to the data area at byte 65536; EOF ends the
last cell. PC cells are zlib streams; Xbox cells are u32 LE usize, u32 LE psize, XMem frames
(empty cells are 6 decoded bytes packed into 40). The PC FoliageMap reader (0x800320 ->
0x7ff650) inflates each cell with zlib (0x84fd80); left as XMem it read garbage group counts
and overran two 256-entry stack arrays at 0x7ff7d3 just after "Game Started".
Decoded cell bytes are kept exactly; only the compression and the offset table change.
Usage: python dlc_foliage.py SOURCE_DATA_PAK OUTPUT_PAK
"""
import pathlib
import struct
import sys
import zlib
from pak import read_pak, file_data
from dlc_unpack import names_of
from dlc_convert import write_pak
from xmem import xmem_decompress

TABLE = 65536
CELLS = TABLE // 4


def xbox_cell(cell):
    """Decoded bytes of an Xbox cell, or None if it is not one."""
    if len(cell) < 11:
        return None
    usize, psize = struct.unpack_from('<II', cell)
    if psize + 8 != len(cell):
        return None
    decoded = xmem_decompress(cell)
    assert len(decoded) == usize
    return decoded


def convert_cells(data):
    starts = list(struct.unpack_from('>' + str(CELLS) + 'I', data))
    assert starts[0] == 0 and starts == sorted(set(starts)) and TABLE + starts[-1] < len(data)
    cells, new_starts, offset, converted = [], [], 0, 0
    for i, (start, end) in enumerate(zip(starts, starts[1:] + [len(data) - TABLE])):
        original = data[TABLE + start:TABLE + end]
        decoded = xbox_cell(original)
        if decoded is not None:
            packed = zlib.compress(decoded, 6)
            assert zlib.decompress(packed) == decoded
            converted += 1
        else:
            zlib.decompress(original)          # fail closed on an unknown format
            packed = original
        new_starts.append(offset)
        cells.append(packed)
        offset += len(packed)
    result = struct.pack('>' + str(CELLS) + 'I', *new_starts) + b''.join(cells)
    assert len(result) == TABLE + offset
    return result, converted


def convert(source, destination):
    data, entries = read_pak(source)
    replacements = {}
    for name, entry in names_of(data, entries):
        if name.startswith('terrain/') and name.endswith('/foliage.raw'):
            changed, count = convert_cells(file_data(data, entry, decode_xmem=True))
            assert count, 'Already converted; no output written'
            replacements[entry['hash']] = changed
            print(name, count, 'of', CELLS, 'cells converted and individually round-trip verified', flush=True)
    assert replacements
    output = []
    for entry in entries:
        h = entry['hash']
        if h in replacements:
            payload = replacements[h]
            output.append((h, 0, payload, len(payload)))
        else:
            output.append((h, entry['method'], data[entry['offset']:entry['offset'] + entry['packed']], entry['size']))
    write_pak(destination, output)
    result, check = read_pak(destination)
    assert len(check) == len(entries)
    for old, new in zip(entries, check):
        assert old['hash'] == new['hash']
        payload = result[new['offset']:new['offset'] + new['packed']]
        if old['hash'] in replacements:
            assert new['method'] == 0 and payload == replacements[old['hash']]
        else:
            assert old['method'] == new['method'] and old['size'] == new['size']
            assert payload == data[old['offset']:old['offset'] + old['packed']]
    print('Verified entire output archive; unrelated entries unchanged', flush=True)


if __name__ == '__main__':
    assert pathlib.Path(sys.argv[1]).resolve() != pathlib.Path(sys.argv[2]).resolve()
    convert(sys.argv[1], sys.argv[2])
