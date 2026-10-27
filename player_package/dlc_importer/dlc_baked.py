"""Convert a DLC pack's baked meshes (bakedmeshes<N>/*.raw) from Xbox XMem to PC zlib blocks.

Without this, starting a DLC track crashed on a loader thread: the baked mesh streaming job
(mt::graphics::PolygonMeshCached, 0x870290) inflates each block with CompressorZLIB, gets
Z_DATA_ERROR (-3) from the Xbox data, and memcpy's -3 bytes into the vertex/index buffer.

A baked mesh file is a short header followed by sub-meshes (shader name, two f32 LOD distances,
u8, then the vertex and index blocks). Only the blocks differ between the platforms:
    PC:   u32 size, zlib stream (size bytes)
    Xbox: u32 size, u32 uncompressed size, u32 packed size (= size - 8), XMem frames
Every block is found by that self-describing Xbox header (sizes must agree and the XMem data must
decode to exactly the stated size) and replaced by the PC form; all other bytes are kept. The
entries stay stored (method 0) like the PC game's, because the job reads blocks at file offsets.

Run after dlc_vtex.py (which rebuilds data.pak from data.pak.before-vtex).
Usage: python tools/dlc_baked.py <pack folder name> [<dlc_pc root>]
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak                             # noqa: E402
from dlc_unpack import names_of                      # noqa: E402
from dlc_events import entry_bytes, rewrite_pak       # noqa: E402
from xmem import xmem_decompress                     # noqa: E402


def xbox_block_at(b, o):
    """Decoded bytes of an Xbox block starting at o (its u32 size), or None."""
    if o + 12 > len(b):
        return None
    size, usize, psize = struct.unpack_from("<III", b, o)
    if psize != size - 8 or not 0 < usize < 64 << 20 or o + 4 + size > len(b) or psize < 4:
        return None
    try:
        data = xmem_decompress(b[o + 4:o + 4 + size])
    except Exception:
        return None
    return data if len(data) == usize else None


def convert(b):
    """(converted bytes, blocks converted). Bytes outside Xbox blocks are copied unchanged."""
    out, o, last, n = bytearray(), 0, 0, 0
    while o + 12 <= len(b):
        data = xbox_block_at(b, o)
        if data is None:
            o += 1
            continue
        packed = zlib.compress(data, 9)
        out += b[last:o] + struct.pack("<I", len(packed)) + packed
        o += 4 + struct.unpack_from("<I", b, o)[0]
        last = o
        n += 1
    out += b[last:]
    return bytes(out), n


def main(pack, root=r"O:\trialsevo\dlc_pc"):
    pak = os.path.join(root, pack, "data", "data.pak")
    d, files = read_pak(pak)
    new = {}
    for name, f in names_of(d, files):
        if not (name.startswith("bakedmeshes") and name.endswith(".raw")):
            continue
        data = entry_bytes(d, f)
        converted, n = convert(data)
        if n:
            new[name] = converted
            print(f"{name}: {n} blocks, {len(data)} -> {len(converted)} bytes")
        else:
            print(f"{name}: no Xbox blocks (already converted?)")

    def edit(name, f, data):
        if name in new:
            return name, 0, new[name], len(new[name])
        return None
    print(f"data.pak: {rewrite_pak(pak, edit)} entries rewritten")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(*sys.argv[1:])
