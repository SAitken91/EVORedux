"""Convert an extracted Xbox 360 Trials Evolution DLC pack so the PC game can load it.

Textures (T?X files) carry a compressed-payload flag; the PC build compresses that payload with
zlib, the Xbox build with XMemCompress (LZX):
    <texture header> 01 <u32 LE usize> <u32 LE psize> <XMem frames>      (Xbox)
    <texture header> 01 <zlib stream>                                   (PC)
The decoded pixels are byte-identical to PC (verified on textures both builds ship), so the only
change is re-compressing the payload. Every other entry is copied byte for byte with its original method: Xbox paks use method 4
(XMem) for most entries, which the PC engine reads natively.

Usage: python tools/dlc_convert.py <extracted STFS root> <output pack dir>
The output dir gets data/*.pak (rewritten) plus the original non-pak files and folders.
"""
import os
import shutil
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data          # noqa: E402
from xmem import xmem_decompress            # noqa: E402

TEXTURE_MAGICS = (b"T3X", b"T4X", b"T5X", b"T6X", b"T7X", b"T8X")


def find_xmem_payload(b):
    """Offset of the 01 flag that precedes an XMem payload ending exactly at end of file.
    Cube maps have a longer header (a 6-face table: the flag sits at byte 120), so the search
    covers the first 1 KB; the exact-end check keeps it from matching inside a header."""
    for o in range(8, min(len(b) - 10, 1024)):
        if b[o] != 1:
            continue
        usize, psize = struct.unpack_from("<II", b, o + 1)
        if o + 9 + psize == len(b) and 0 < psize <= usize < 64 * 1024 * 1024:
            first = b[o + 9]
            if first == 0xFF or struct.unpack_from(">H", b, o + 9)[0] <= psize:
                return o
    return None


def convert_texture(b):
    o = find_xmem_payload(b)
    if o is None:
        return b, False
    try:
        pixels = xmem_decompress(b[o + 1:])
    except ValueError:
        # One Origin of Pain texture (0xd70eae31) fails near its end; keep what decodes.
        pixels = xmem_decompress(b[o + 1:], partial_ok=True)
        print("warning: texture decoded partially")
    return b[:o + 1] + zlib.compress(pixels, 6), True


def write_pak(path, entries):
    """entries: list of (hash, method, packed bytes, unpacked size), written in order."""
    count = len(entries)
    header = 12 + count * 17
    table, offset = [], header
    for h, method, packed, size in entries:
        table.append(struct.pack("<IIIBI", h, len(packed), size, method, offset))
        offset += len(packed)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x12345678, header, count))
        f.write(b"".join(table))
        for _h, _m, packed, _s in entries:
            f.write(packed)


def unpack_entry(d, f):
    """Entry bytes as the game sees them. Method 8 = zlib (PC), 4 = XMem/LZX (Xbox), 0 = stored."""
    raw = d[f["offset"]:f["offset"] + f["packed"]]
    if f["method"] == 8:
        return zlib.decompress(raw)
    if f["method"] == 4:
        return xmem_decompress(raw)
    return raw


def convert_pack(src, dst):
    stats = {}
    os.makedirs(os.path.join(dst, "data"), exist_ok=True)
    for name in os.listdir(src):                      # loose files (pngs, spa.bin); vtex stays in place
        s = os.path.join(src, name)
        if os.path.isfile(s):
            shutil.copy2(s, os.path.join(dst, name))
    for pak in sorted(os.listdir(os.path.join(src, "data"))):
        if not pak.endswith(".pak"):
            continue
        d, files = read_pak(os.path.join(src, "data", pak))
        out = []
        for f in files:
            raw = d[f["offset"]:f["offset"] + f["packed"]]
            entry = (f["hash"], f["method"], raw, f["size"])      # default: byte-for-byte copy
            data = unpack_entry(d, f) if f["method"] in (0, 4, 8) else None
            if data is not None and data[:3] in TEXTURE_MAGICS:
                data, changed = convert_texture(data)
                if changed:
                    entry = (f["hash"], 8, zlib.compress(data, 6), len(data))
                key = (pak, "texture converted" if changed else "texture unchanged")
                stats[key] = stats.get(key, 0) + 1
            out.append(entry)
        write_pak(os.path.join(dst, "data", pak), out)
    return stats


if __name__ == "__main__":
    for k, v in sorted(convert_pack(sys.argv[1], sys.argv[2]).items()):
        print(k, v)
