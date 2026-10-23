"""Unpack an extracted Xbox 360 Trials Evolution DLC into plain files (paks + XMem/LZX layer).

Usage: python tools/dlc_unpack.py <extracted STFS root> <out dir>
The pak name tables live in entry 0xdd9d93e8 (u32 count + u16-length names, entry order).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data          # noqa: E402
from xmem import xmem_decompress, is_xmem   # noqa: E402

NAME_TABLE = 0xdd9d93e8


def names_of(d, files):
    nl = [f for f in files if f["hash"] == NAME_TABLE]
    if not nl:
        return [(f"#{f['hash']:08x}", f) for f in files]
    b = file_data(d, nl[0])
    count, o, names = struct.unpack_from("<I", b, 0)[0], 4, []
    while len(names) < count and o + 2 <= len(b):
        n = struct.unpack_from("<H", b, o)[0]
        names.append(b[o + 2:o + 2 + n].decode("latin-1"))
        o += 2 + n
    rest = [f for f in files if f is not nl[0]]
    return list(zip(names, rest))


def unpack(root, out_dir):
    stats, fails = {}, []
    for pak in ("configs.pak", "data.pak", "data_misc.pak", "data_tex.pak"):
        path = os.path.join(root, "data", pak)
        if not os.path.exists(path):
            continue
        d, files = read_pak(path)
        for name, f in names_of(d, files):
            b, kind = file_data(d, f), "raw"
            if is_xmem(b):
                try:
                    b, kind = xmem_decompress(b), "xmem"
                except Exception as e:                # keep the packed bytes if decoding fails
                    fails.append((name, str(e)))
                    kind = "xmem-fail"
            key = (pak, kind)
            stats[key] = stats.get(key, 0) + 1
            dest = os.path.join(out_dir, *name.replace("\\", "/").split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as w:
                w.write(b)
    return stats, fails


if __name__ == "__main__":
    stats, fails = unpack(sys.argv[1], sys.argv[2])
    print(stats)
    for name, err in fails[:20]:
        print("FAILED", name, err)
