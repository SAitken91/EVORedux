"""Convert a DLC pack's virtual textures (vtex) and decal data from Xbox XMem to PC zlib.

Run after dlc_convert.py. Without this, loading a DLC track hangs on the loading screen: the PC
reads every vtex block / decal cell through zlib, gets garbage from the Xbox XMem data, and
VTBlockRendererGame::loadTriangles (0x8098d0) asks operator new for an impossible size, which
retries forever.

Formats (same on both platforms apart from the compression):
  vtex/<name>.raw.hdr (in data.pak)  u32 count, count x 11 bytes:
        u32 page id, u32 offset, u16 size, u8 file
        offset and size are in 4-byte units (blocks are padded to 4 bytes); offset bit 31 marks a
        page that reuses another entry's block; file 0xYX -> <prefix>_X_Y.raw, 0xFE/0xFF -> the
        upper (low-detail) file. Verified against every PC .raw size.
  vtex/*.raw (loose, next to data/)  the blocks: PC = zlib stream, Xbox = bare XMem frames
  vtex/decals*.dat (in data.pak)     u32 0x800, u32 0x80, then cells: u8 0 (empty) or
        u8 1, u32 size, data (PC = zlib stream, Xbox = u32 usize, u32 psize, XMem frames)
Decoded bytes are identical on both platforms (little-endian), so only the compression changes.

Usage: python tools/dlc_vtex.py <pack folder name> [<extracted root>] [<dlc_pc root>]
Writes <dlc_pc root>/<pack>/vtex/*.raw and rewrites the .raw.hdr / decals entries in data.pak.
"""
import os
import shutil
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak                             # noqa: E402
from dlc_unpack import names_of                      # noqa: E402
from dlc_events import entry_bytes, rewrite_pak       # noqa: E402
from xmem import xmem_decompress, is_xmem            # noqa: E402

FRAME = 32768


def split_frames(block):
    """(frames, uncompressed size) of a bare XMem frame list followed by zero padding."""
    o, usize = 0, 0
    while True:
        rest = block[o:]
        if o and not any(rest):                     # zero padding after the last frame
            return block[:o], usize
        if o + 2 > len(block):
            raise ValueError("vtex block is not XMem frames")
        if block[o] == 0xFF:
            usize += struct.unpack_from(">H", block, o + 1)[0]
            o += 3
        else:
            usize += FRAME
        o += 2 + struct.unpack_from(">H", block, o)[0]
        if o > len(block):
            raise ValueError("vtex block is not XMem frames")


def decode_block(block):
    frames, usize = split_frames(block)
    packed = struct.pack("<II", usize, len(frames)) + frames
    try:
        return xmem_decompress(packed)
    except ValueError:
        # One Riders of Doom object page (vtex_objects_2_5.raw) fails near its end; keep what
        # decodes (rest zero-filled), as for the one partial DLC texture.
        print("  warning: vtex block decoded partially")
        return xmem_decompress(packed, partial_ok=True)


def zlib_small(data, limit):
    for level in (6, 9):
        z = zlib.compress(data, level)
        if len(z) <= limit:
            return z
    raise ValueError(f"block of {len(data)} bytes does not fit in {limit} bytes compressed")


def raw_file_name(hdr_name, code, src_dir):
    stem = hdr_name[len("vtex/"):-len(".raw.hdr")]           # vtex_terrain2 / vtex_objects2
    kind = "terrain" if "terrain" in stem else "objects"
    if code in (0xFE, 0xFF):
        candidates = [f"vtex_upper_{stem[len('vtex_'):]}.raw", f"vtex_upper_{kind}.raw"]
    else:
        x, y = code & 15, code >> 4
        candidates = [f"{stem}_{x}_{y}.raw", f"vtex_{kind}_{x}_{y}.raw"]
    for c in candidates:
        if os.path.exists(os.path.join(src_dir, c)):
            return c
    raise FileNotFoundError(f"{hdr_name}: no raw file for code {code:#04x} (tried {candidates})")


def convert_hdr(hdr_name, hdr, src_dir, out_dir):
    count = struct.unpack_from("<I", hdr, 0)[0]
    entries = [list(struct.unpack_from("<IIHB", hdr, 4 + i * 11)) for i in range(count)]
    by_file = {}
    for e in entries:
        by_file.setdefault(e[3], []).append(e)
    for code, group in sorted(by_file.items()):
        name = raw_file_name(hdr_name, code, src_dir)
        src = open(os.path.join(src_dir, name), "rb").read()
        new_offset, out = {}, bytearray()
        for off, size in sorted({(e[1] & 0x7FFFFFFF, e[2]) for e in group}):
            block = src[off * 4:(off + size) * 4]
            if size:
                z = zlib_small(decode_block(block), 0xFFFF * 4 - 3)
                z += bytes(-len(z) % 4)
            else:
                z = b""
            new_offset[(off, size)] = (len(out) // 4, len(z) // 4)
            out += z
        for e in group:
            flag = e[1] & 0x80000000
            e[1], e[2] = new_offset[(e[1] & 0x7FFFFFFF, e[2])]
            e[1] |= flag
        with open(os.path.join(out_dir, name), "wb") as w:
            w.write(out)
        print(f"  {name}: {len(src)} -> {len(out)} bytes, {len(new_offset)} blocks")
    return struct.pack("<I", count) + b"".join(struct.pack("<IIHB", *e) for e in entries)


def convert_decals(dat):
    out, o = bytearray(dat[:8]), 8
    while o < len(dat):
        if dat[o] == 0:
            out.append(0)
            o += 1
            continue
        if dat[o] != 1:
            raise ValueError(f"decals: unexpected cell flag {dat[o]} at {o}")
        size = struct.unpack_from("<I", dat, o + 1)[0]
        cell = dat[o + 5:o + 5 + size]
        if is_xmem(cell):
            cell = zlib.compress(xmem_decompress(cell), 6)
        out += b"\x01" + struct.pack("<I", len(cell)) + cell
        o += 5 + size
    return bytes(out)


def main(pack, src_root=r"O:\trialsevo\dlc", out_root=r"O:\trialsevo\dlc_pc"):
    src_dir = os.path.join(src_root, pack, "vtex")
    out_dir = os.path.join(out_root, pack, "vtex")
    os.makedirs(out_dir, exist_ok=True)
    pak = os.path.join(out_root, pack, "data", "data.pak")
    # The Xbox index/decal entries are always read from the untouched copy, so a rerun converts
    # the same input again instead of reinterpreting already converted offsets.
    original = pak + ".before-vtex"
    if not os.path.exists(original):
        shutil.copy2(pak, original)
    shutil.copy2(original, pak)
    d, files = read_pak(pak)
    new = {}
    for name, f in names_of(d, files):
        if not name.startswith("vtex/"):
            continue
        data = entry_bytes(d, f)
        if name.endswith(".raw.hdr"):
            print(name)
            new[name] = convert_hdr(name, data, src_dir, out_dir)
        elif name.endswith(".dat"):
            converted = convert_decals(data)
            if converted != data:
                new[name] = converted
                print(f"{name}: decal cells recompressed")

    def edit(name, f, data):
        if name in new:
            return name, 8, zlib.compress(new[name], 9), len(new[name])
        return None
    print(f"data.pak: {rewrite_pak(pak, edit)} entries rewritten")


if __name__ == "__main__":
    main(*sys.argv[1:])
