"""Turn the DLC worlds' base terrain layer materials into ordinary decal materials.

Each pack's objectcollection<N>.xml marks its base terrain layers (Material/Terrain/BaseTerrain/
Materials/*: 13 in Origin of Pain, 4 in Riders of Doom) with tiledAtlas = 1..13: on the Xbox 360 a
separate pass (vtPageTerrainBase) drew them from a tiled atlas. The PC has no such pass. Its atlas
rebuild (0x78b5a0) visits every material twice, tiled pass first, and clears every texture slot at
the start of each pass, so a tiledAtlas material gets its images in the tiled pass and loses them in
the normal one. The terrain decal renderer then finds those materials without images and draws one
shared atlas tile. With tiledAtlas = 0 they take the normal pass, like every other DLC ground
material, and keep their images in the decal atlas (0x786d60 and 0x78ad47, the only other readers,
make the same tiled/normal choice).

Only needed with the baked base layer (dlc_terrain_base.py): the DLC decal tables reference these
materials only through it. dlc_terrain_base.py takes the layer order from tiledAtlas, so bake from
an archive that has not been through this script.

Usage: python dlc_untile_terrain.py SOURCE_PAK OUTPUT_PAK <world 1|2>
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data                    # noqa: E402
from dlc_unpack import names_of                        # noqa: E402
from dlc_convert import write_pak                      # noqa: E402

TILED_ATLAS = struct.pack("<I", 0x29ecf4e5)            # name hash of "tiledAtlas"
STRING = 6                                             # property type: u16 length, bytes (NUL included)


def untile(xml):
    """Set every tiledAtlas string property to "0" (length kept). Returns (new bytes, count)."""
    out = bytearray(xml)
    count, i = 0, out.find(TILED_ATLAS)
    while i != -1:
        if out[i + 4] == STRING:
            n = struct.unpack_from("<H", out, i + 5)[0]
            value = bytes(out[i + 7:i + 7 + n])
            digits = value.rstrip(bytes(1))
            assert n >= 2 and value[-1] == 0 and digits.isdigit(), value
            if int(digits) != 0:
                out[i + 7:i + 7 + n] = b"0" + b"\0" * (n - 1)
                count += 1
        i = out.find(TILED_ATLAS, i + 1)
    return bytes(out), count


def main(source, output, world):
    d, files = read_pak(source)
    named = dict(names_of(d, files))
    target = named["objectcollection%d.xml" % world]
    entries, changed = [], 0
    for f in files:
        raw = d[f["offset"]:f["offset"] + f["packed"]]
        if f is target:
            new, changed = untile(file_data(d, f, decode_xmem=True))
            entries.append((f["hash"], 8, zlib.compress(new, 9), len(new)))
        else:
            entries.append((f["hash"], f["method"], raw, f["size"]))
    if not changed:
        sys.exit("no tiledAtlas materials found (already untiled?)")
    write_pak(output, entries)
    d2, files2 = read_pak(output)                      # verify: target changed, everything else intact
    assert len(files2) == len(files)
    for f, g in zip(files, files2):
        if f is target:
            assert untile(file_data(d2, g))[1] == 0
        else:
            assert d[f["offset"]:f["offset"] + f["packed"]] == d2[g["offset"]:g["offset"] + g["packed"]]
    print(f"{output}: {changed} base terrain layer materials untiled")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]))
