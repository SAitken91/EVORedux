"""Copy base-game particle materials that DLC effects reference into the DLC object collections.

DLC fire effects (fire_area_massive_*, fire_2_*, firebomb_*, verticalfire_* ...) use the
materials Fire_2_sequence and Glow_1, which only exist in the PC base objectcollection.xml.
In a DLC world the engine resolves materials from the DLC collection, so the flames never
render (only the DLC-defined smoke shows). This copies the two material nodes (with their
unnamed wrapper) from the base collection into DLC1/DLC2 Material/Effects/Particles.

Usage: python tools/dlc_fire_materials.py <TrialsPC dir>   (game must be closed)
Backs up each pak as <pak>.before-fire-materials once.
"""
import os
import shutil
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak                              # noqa: E402
from dlc_convert import write_pak, unpack_entry       # noqa: E402
from dlc_unpack import names_of                       # noqa: E402

NAME = 0xca82e140
BASE_COLLECTION = 1796245815                          # objectcollection.xml in build/data/data.pak
WANTED = ("Fire_2_sequence", "Glow_1")
PACKS = (("OriginofPain", "objectcollection1.xml"), ("RidersofDoom", "objectcollection2.xml"))


def parse_node(b, i):
    assert b[i:i + 2] == b"\x01\x00", i
    start, h = i, struct.unpack_from("<I", b, i + 2)[0]
    i += 6
    props = []
    while b[i] == 1:
        ph, t = struct.unpack_from("<I", b, i + 2)[0], b[i + 6]
        n = struct.unpack_from("<H", b, i + 7)[0]
        props.append((ph, t, b[i + 9:i + 9 + n]))
        i += 9 + n
    assert b[i] == 0, i
    i += 1
    kids = []
    while b[i] == 1:
        k, i = parse_node(b, i + 1)
        kids.append(k)
    assert b[i] == 0, i
    return dict(h=h, props=props, kids=kids, s=start, e=i + 1), i + 1


def name(n):
    for ph, _t, v in n["props"]:
        if ph == NAME:
            return v.rstrip(b"\0").decode("latin-1")


def walk(n, path=()):
    p = path + (name(n),)
    yield n, p
    for k in n["kids"]:
        yield from walk(k, p)


def load(b):
    root, end = parse_node(b, 0)
    assert end == len(b), "collection did not parse to its end"
    return root


def find_wrappers(root):
    """Unnamed parent node of each wanted material (the node that is copied)."""
    out = {}
    for n, _p in walk(root):
        for k in n["kids"]:
            if name(k) in WANTED and name(n) is None:
                out[name(k)] = n
    return out


def patch(dlc, base_wrappers, tag):
    root = load(dlc)
    existing = {p[-1] for _n, p in walk(root)}
    particles = [n for n, p in walk(root) if p[-4:] == (tag, "Material", "Effects", "Particles")]
    assert len(particles) == 1, "DLC Material/Effects/Particles not found"
    parent = particles[0]
    add = [w for nm, w in base_wrappers.items() if nm not in existing]
    if not add:
        return None
    blob = b"".join(b"\x01" + BASE[w["s"]:w["e"]] for w in add)
    pos = parent["e"] - 1                               # before the parent's child terminator
    assert dlc[pos] == 0
    out = dlc[:pos] + blob + dlc[pos:]
    load(out)
    return out, [name(w["kids"][0]) for w in add]


def main(game):
    global BASE
    d, files = read_pak(os.path.join(game, "build", "data", "data.pak"))
    BASE = unpack_entry(d, next(f for f in files if f["hash"] == BASE_COLLECTION))
    wrappers = find_wrappers(load(BASE))
    assert set(wrappers) == set(WANTED), wrappers.keys()
    for pack, coll in PACKS:
        for pak in ("configs.pak", "data.pak", "data_misc.pak"):
            path = os.path.join(game, "datapack", "DLCContent", pack, "data", pak)
            if not os.path.exists(path):
                continue
            d, files = read_pak(path)
            named = names_of(d, files)
            hit = [f for n, f in named if n.lower().replace("\\", "/").endswith(coll)]
            if not hit:
                continue
            raw = unpack_entry(d, hit[0])
            if raw[:2] != b"\x01\x00":                  # extra DLC layer: u32 usize, u32 csize, XMem
                from xmem import xmem_decompress
                raw = xmem_decompress(raw[8:])
            res = patch(raw, wrappers, "DLC1" if coll.endswith("1.xml") else "DLC2")
            if res is None:
                print(pack, pak, "already has the materials")
                continue
            new, added = res
            bak = path + ".before-fire-materials"
            if not os.path.exists(bak):
                shutil.copy2(path, bak)
            entries = []
            for f in files:
                if f is hit[0]:
                    entries.append((f["hash"], 8, zlib.compress(new, 9), len(new)))
                else:
                    entries.append((f["hash"], f["method"], d[f["offset"]:f["offset"] + f["packed"]], f["size"]))
            write_pak(path, entries)
            print(pack, pak, coll, "added", added, len(raw), "->", len(new))


if __name__ == "__main__":
    main(sys.argv[1])
