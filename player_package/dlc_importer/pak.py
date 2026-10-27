"""Trials Evolution .pak reader (same container on PC and Xbox 360).

Layout: u32 magic 0x12345678, u32 data_start, u32 count, count x 17-byte entries
(u32 name hash, u32 packed size, u32 size, u8 method (8 = zlib, 4 = XMem, 0 = stored), u32 offset), file data,
then (when present) a name table of (u16 len, name) in entry order.

Usage: python tools/pak.py <file.pak> [--list] [--extract <dir>]
"""
import os
import struct
import sys
import zlib


def read_pak(path):
    d = open(path, "rb").read()
    magic, _data_start, count = struct.unpack_from("<III", d, 0)
    if magic != 0x12345678:
        raise ValueError(f"{path}: not a Trials pak")
    entries = [struct.unpack_from("<IIIBI", d, 12 + i * 17) for i in range(count)]
    end = max((off + packed for _h, packed, _s, _m, off in entries), default=12 + count * 17)
    names = []
    o = end
    while len(names) < count and o + 2 <= len(d):
        n = struct.unpack_from("<H", d, o)[0]
        names.append(d[o + 2:o + 2 + n].decode("latin-1"))
        o += 2 + n
    if len(names) != count:
        names = [f"#{h:08x}" for h, *_ in entries]           # name table absent: hashes only
    files = []
    for name, (h, packed, size, method, off) in zip(names, entries):
        files.append({"name": name, "hash": h, "size": size, "packed": packed, "method": method,
                      "offset": off})
    return d, files


def file_data(d, f, decode_xmem=False):
    """Entry bytes. Method 8 = zlib (PC). Method 4 = XMem/LZX (Xbox 360 DLC): returned still packed
    (u32 usize, u32 psize, frames) unless decode_xmem, which needs tools/xmem.py."""
    raw = d[f["offset"]:f["offset"] + f["packed"]]
    if f["method"] == 8:
        return zlib.decompress(raw)
    if f["method"] == 4 and decode_xmem:
        from xmem import xmem_decompress
        return xmem_decompress(raw)
    return raw


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d, files = read_pak(sys.argv[1])
    if "--extract" in sys.argv:
        out = sys.argv[sys.argv.index("--extract") + 1]
        for f in files:
            dest = os.path.join(out, *f["name"].split("/"))
            os.makedirs(os.path.dirname(dest) or out, exist_ok=True)
            with open(dest, "wb") as w:
                w.write(file_data(d, f))
        print(f"extracted {len(files)} files to {out}")
    else:
        for f in files:
            print(f"{f['size']:>10}  {f['name']}")
