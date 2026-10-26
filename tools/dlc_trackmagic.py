"""Give DLC tracks the PC track magic (0xCAFEB00B -> 0xDEADBABE).

Track files (tracks/*.trk) start with a u32 magic. The PC loader only accepts 0xDEADBABE
(checks at 0x4e9ed9, 0x4eb3f9, 0x4eb7a9, 0x4ebe96) and otherwise stops before reading any chunk,
so the track never loads (every Riders of Doom track, and 6 Origin of Pain ones). The Xbox DLC
uses 0xCAFEB00B on 42 of its 86 tracks; header layout (version 4, header length, user id),
LZMA stream position and contents are otherwise identical to 0xDEADBABE tracks, with no extra
trailing data, so only the four magic bytes change.
Usage: python dlc_trackmagic.py SOURCE_DATA_PAK OUTPUT_PAK
"""
import lzma
import pathlib
import struct
import sys
from pak import read_pak, file_data
from dlc_unpack import names_of
from dlc_convert import write_pak

PC_MAGIC = bytes.fromhex('deadbabe')
DLC_MAGIC = bytes.fromhex('cafeb00b')


def check_track(b):
    """Decode the track's LZMA body (fails closed if the layout isn't the expected one)."""
    i = b.find(b'\x5d\x00\x00\x02\x00')
    assert i == 37 and b[4:8] == b'\x04\x00\x00\x00', 'unexpected track header'
    usize = struct.unpack_from('<I', b, i + 5)[0]
    dec = lzma.LZMADecompressor(lzma.FORMAT_ALONE)
    out = dec.decompress(b[i:i + 5] + struct.pack('<q', usize) + b[i + 9:])
    assert len(out) == usize and not dec.unused_data and out.endswith(b'END0\x00\x00\x00\x00')


def convert(source, destination):
    data, entries = read_pak(source)
    replacements = {}
    for name, entry in names_of(data, entries):
        if not (name.startswith('tracks/') and name.endswith('.trk')):
            continue
        b = file_data(data, entry, decode_xmem=True)
        if b[:4] != DLC_MAGIC:
            assert b[:4] == PC_MAGIC, (name, b[:4].hex())
            continue
        check_track(b)
        replacements[entry['hash']] = (entry['method'], PC_MAGIC + b[4:])
    assert replacements, 'No 0xCAFEB00B tracks; input left untouched'
    print(len(replacements), 'tracks rewritten to', PC_MAGIC.hex())
    output = []
    for entry in entries:
        h = entry['hash']
        if h in replacements:
            method, payload = replacements[h]
            assert method == 0, 'expected stored track entries'
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
            assert payload == replacements[old['hash']][1]
        else:
            assert old['method'] == new['method'] and old['size'] == new['size']
            assert payload == data[old['offset']:old['offset'] + old['packed']]
    print('Verified entire output archive; unrelated entries unchanged', flush=True)


if __name__ == '__main__':
    assert pathlib.Path(sys.argv[1]).resolve() != pathlib.Path(sys.argv[2]).resolve()
    convert(sys.argv[1], sys.argv[2])
