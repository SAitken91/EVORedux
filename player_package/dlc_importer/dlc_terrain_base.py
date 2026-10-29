"""Bake the DLC worlds' base terrain layers into their terrain decal tables (decalsTerrain<N>.dat).

The Xbox 360 title update that added the DLC paints a base layer into every terrain virtual-texture
page before the decals (shader vtPageTerrainBase: up to 16 ground materials blended in order, each
lerped in by its weight map). The PC build has no such pass: the PC base world has its base layer
baked into decalsTerrain.dat instead (every cell holds big linear-UV decals with per-vertex alpha).
The DLC decal tables only hold the extra decals, so on PC the DLC ground came out flat or, before
the shim's page clear, as leftovers of other pages (Buccaneer Bay's seabed).

This bakes the 360 base layer the PC way. Inputs, all from the installed pack:
  objectcollection<N>.xml  Material/Terrain/BaseTerrain/Materials/*: the layer materials (layer index
                           attribute 0x29ecf4e5 = 1..13; decal record id = name hash of the
                           material number)
  data_tex.pak             <id>_comb01..comb1011_default.tex: the 12 weight maps (BaseMaterial_
                           weights, terrainBase.sd), 2048x2048 BC5, two layers per texture
  vtex/decalsTerrain<N>.dat
Each 2048-unit cell (32x32, coordinates relative to the cell centre, 16 units per metre) gets, before
its own decals, one linear-UV decal per layer: an adaptive quad mesh whose vertex alpha is the layer
weight (sampled bilinearly), so the decal shader's alpha blend reproduces the 360's sequential lerps.
UVs tile every 128 units for every layer, as the 360 shader does (frac(uv * 512) over the terrain);
the material's own uvscale is divided out.

Usage: python dlc_terrain_base.py <TrialsPC dir>      (game closed; backs up data.pak once)
"""
import os
import shutil
import struct
import sys
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data                    # noqa: E402
from dlc_unpack import names_of                        # noqa: E402
from dlc_convert import write_pak                      # noqa: E402
from dlc_fire_materials import load, walk, name        # noqa: E402

PACKS = (("OriginofPain", 1), ("RidersofDoom", 2))
LAYER = 0x29ecf4e5
KEY = 0x255b12
VALUE = 0xd72d5372
CELLS, CELL = 32, 2048              # decal grid and cell size (terrain texels)
WEIGHT_SIZE = 2048                  # weight map texels across the terrain
TILE = 128.0                        # one material repeat (360: 512 repeats over 65536 texels)
STEP = 32                           # finest quad size
BLOCK = 8                           # quads per decal side (8 x STEP texels): 81 vertices, 384 indices max
BACKUP = ".before-terrain-base"


def name_hash(s):
    """Material library key (0x7971a0): hash of the material name string."""
    b = s.encode("latin-1")
    m, acc = len(b) & 0xffff, 0
    for c in b:
        c = c - 256 if c >= 128 else c
        acc = (acc + c * m) & 0xffffffff
        m = ((m >> 16) + (m & 0xffff) * 18000) & 0xffffffff
    return acc


def prop(n, h):
    for ph, _t, v in n["props"]:
        if ph == h:
            return v
    return None


def layer_materials(collection):
    """[(layer index 0.., record id, uvscale)] from Material/Terrain/BaseTerrain/Materials."""
    root = load(collection)
    out = []
    for n, path in walk(root):
        if "Materials" not in path[-3:-1] or "BaseTerrain" not in path:
            continue
        key = prop(n, KEY)
        if not key or len(key) != 4:
            continue
        layer, uvscale = None, 1.0
        stack = list(n["kids"])
        while stack:
            k = stack.pop()
            v = prop(k, LAYER)
            if v is not None:
                layer = int(v.rstrip(b"\0"))
            if name(k) == "uvscale" and prop(k, VALUE):
                uvscale = float(prop(k, VALUE).rstrip(b"\0"))
            stack += k["kids"]
        if layer:
            num = struct.unpack("<I", key)[0]
            out.append((layer - 1, name_hash(str(num)), uvscale, path[-1]))
    return sorted(out)


def bc4(blk):
    bh, bw = blk.shape[:2]
    a0, a1 = blk[..., 0].astype(np.int32), blk[..., 1].astype(np.int32)
    bits = np.zeros((bh, bw), np.uint64)
    for i in range(6):
        bits |= blk[..., 2 + i].astype(np.uint64) << np.uint64(8 * i)
    pal = np.zeros((bh, bw, 8), np.int32)
    pal[..., 0], pal[..., 1] = a0, a1
    big = a0 > a1
    for k in range(1, 7):
        pal[..., 1 + k] = ((7 - k) * a0 + k * a1) // 7
    for k in range(1, 5):
        pal[..., 1 + k] = np.where(big, pal[..., 1 + k], ((5 - k) * a0 + k * a1) // 5)
    pal[..., 6] = np.where(big, pal[..., 6], 0)
    pal[..., 7] = np.where(big, pal[..., 7], 255)
    out = np.zeros((bh * 4, bw * 4), np.uint8)
    for p in range(16):
        idx = ((bits >> np.uint64(3 * p)) & np.uint64(7)).astype(np.int64)
        out[(p // 4)::4, (p % 4)::4] = np.take_along_axis(pal, idx[..., None], 2)[..., 0]
    return out


def weight_maps(tex_pak):
    """12 weight maps (float 0..1, [row, col]) from the single-mip comb01..comb1011 textures."""
    d, e = read_pak(tex_pak)
    found = {}
    for n, f in names_of(d, e):
        base = n.rsplit("/", 1)[-1]
        if "_comb" not in base or not base.endswith("_default.tex"):
            continue
        b = file_data(d, f, decode_xmem=True)
        w, h, fmt, mips = struct.unpack_from("<HHBB", b, 4)
        if (w, h, fmt, mips) != (WEIGHT_SIZE, WEIGHT_SIZE, 34, 1):
            continue
        found[base.split("_comb")[1].split("_")[0]] = b
    order = ("01", "23", "45", "67", "89", "1011")
    assert all(k in found for k in order), "weight maps missing: %s" % sorted(found)
    maps = []
    for k in order:
        b = found[k]
        assert b[30] == 1, "unexpected texture payload"
        px = zlib.decompress(b[31:])
        blk = np.frombuffer(px[:WEIGHT_SIZE * WEIGHT_SIZE], np.uint8).reshape(WEIGHT_SIZE // 4, WEIGHT_SIZE // 4, 16)
        maps += [bc4(blk[..., :8]), bc4(blk[..., 8:])]
    return [m.astype(np.float32) / 255.0 for m in maps]


def cells(dat):
    o, out = 8, []
    while o < len(dat):
        f = dat[o]
        o += 1
        if f == 0:
            out.append(None)
            continue
        assert f == 1
        n = struct.unpack_from("<I", dat, o)[0]
        out.append(zlib.decompress(dat[o + 4:o + 4 + n]))
        o += 4 + n
    assert len(out) == CELLS * CELLS
    return out


def half(v):
    return int(np.array(v, np.float16).view(np.uint16))


def sample(wmap, gx, gy):
    """Bilinear weight at terrain texel positions (arrays)."""
    x = gx / (CELLS * CELL / WEIGHT_SIZE) - 0.5
    y = gy / (CELLS * CELL / WEIGHT_SIZE) - 0.5
    x0 = np.clip(np.floor(x).astype(int), 0, WEIGHT_SIZE - 1)
    y0 = np.clip(np.floor(y).astype(int), 0, WEIGHT_SIZE - 1)
    x1, y1 = np.clip(x0 + 1, 0, WEIGHT_SIZE - 1), np.clip(y0 + 1, 0, WEIGHT_SIZE - 1)
    fx, fy = np.clip(x - x0, 0, 1), np.clip(y - y0, 0, 1)
    return ((wmap[y0, x0] * (1 - fx) + wmap[y0, x1] * fx) * (1 - fy) +
            (wmap[y1, x0] * (1 - fx) + wmap[y1, x1] * fx) * fy)


def quads(grid, x0, y0, size, out):
    """Adaptive quadtree over a (n x n) vertex grid of weights; leaves as (x, y, size) in steps."""
    region = grid[y0:y0 + size + 1, x0:x0 + size + 1]
    if region.max() <= 0.0:
        return
    if size == 1 or region.max() - region.min() < 10 / 255.0:
        out.append((x0, y0, size))
        return
    h = size // 2
    for dy in (0, h):
        for dx in (0, h):
            quads(grid, x0 + dx, y0 + dy, h, out)


def base_records(rec_id, uvscale, wmap, col, row):
    """One decal per BLOCK x BLOCK sub-block of the cell. The page renderer copies each decal into
    dynamic buffers of 0xaab vertices and 0xaab indices (0x8065c0), so a decal must stay well
    under that; the PC base world's largest has 258 vertices / 768 indices."""
    n = CELL // STEP
    local = np.arange(n + 1) * STEP - CELL / 2
    gx = (col + 0.5) * CELL + local[None, :]
    gy = (row + 0.5) * CELL + local[:, None]
    grid = sample(wmap, np.broadcast_to(gx, (n + 1, n + 1)), np.broadcast_to(gy, (n + 1, n + 1)))
    a = 1.0 / (TILE * uvscale)
    uv = struct.pack("<6H", half(0.0), half(0.0), half(a), half(0.0), half(0.0), half(-a))
    out = []
    for by in range(0, n, BLOCK):
        for bx in range(0, n, BLOCK):
            leaves = []
            quads(grid, bx, by, BLOCK, leaves)
            if not leaves:
                continue
            verts, idx, shared = [], [], {}
            for qx, qy, s in leaves:
                corners = []
                for vx, vy in ((qx, qy), (qx + s, qy), (qx + s, qy + s), (qx, qy + s)):
                    if (vx, vy) not in shared:          # corners shared: at most (BLOCK + 1)^2
                        shared[vx, vy] = len(verts)
                        verts.append((local[vx], local[vy], float(grid[vy, vx])))
                    corners.append(shared[vx, vy])
                p, q, r, t = corners
                idx += [p, q, r, p, r, t]
            assert len(verts) <= 300 and len(idx) <= 800, "decal too large for the page renderer"
            rec = struct.pack("<IBIB", rec_id, 0, len(verts), 1) + uv
            rec += b"".join(struct.pack("<HHf", half(x), half(y), w) for x, y, w in verts)
            rec += struct.pack("<I", len(idx) // 3) + struct.pack("<%dH" % len(idx), *idx)
            out.append(rec)
    return out


def bake(dat, layers, weights):
    old = cells(dat)
    out = bytearray(dat[:8])
    added = vertices = 0
    for c in range(CELLS * CELLS):
        row, col = divmod(c, CELLS)
        recs = []
        for layer, rec_id, uvscale, _name in layers:
            if layer >= len(weights):
                continue
            for r in base_records(rec_id, uvscale, weights[layer], col, row):
                recs.append(r)
                vertices += struct.unpack_from("<I", r, 5)[0]
        count, body = 0, b""
        if old[c] is not None:
            count = struct.unpack_from("<I", old[c])[0]
            body = old[c][4:]
        cell = struct.pack("<I", count + len(recs)) + b"".join(recs) + body
        added += len(recs)
        if count + len(recs) == 0:
            out.append(0)
            continue
        z = zlib.compress(cell, 6)
        out += b"\x01" + struct.pack("<I", len(z)) + z
    return bytes(out), added, vertices


def main(game):
    for pack, world in PACKS:
        root = os.path.join(game, "datapack", "DLCContent", pack, "data")
        pak = os.path.join(root, "data.pak")
        if not os.path.exists(pak):
            print(pack, "not installed, skipped")
            continue
        source = pak + BACKUP if os.path.exists(pak + BACKUP) else pak
        d, e = read_pak(source)
        named = dict(names_of(d, e))
        decals_name = "vtex/decalsTerrain%d.dat" % (world + 1)
        layers = layer_materials(file_data(d, named["objectcollection%d.xml" % world], decode_xmem=True))
        weights = weight_maps(os.path.join(root, "data_tex.pak"))
        print(pack, "layers:", ", ".join("%d %s" % (l, n) for l, _r, _s, n in layers))
        dat = file_data(d, named[decals_name], decode_xmem=True)
        new, added, vertices = bake(dat, layers, weights)
        assert cells(new)
        target = named[decals_name]["hash"]
        output = []
        for entry in e:
            raw = d[entry["offset"]:entry["offset"] + entry["packed"]]
            if entry["hash"] == target:
                output.append((entry["hash"], 8, zlib.compress(new, 6), len(new)))
            else:
                output.append((entry["hash"], entry["method"], raw, entry["size"]))
        if source == pak:
            shutil.copyfile(pak, pak + BACKUP)
        tmp = pak + ".tmp"
        write_pak(tmp, output)
        d2, e2 = read_pak(tmp)
        assert file_data(d2, dict(names_of(d2, e2))[decals_name]) == new
        os.replace(tmp, pak)
        print("%s: %s %d -> %d bytes, %d base-layer decals, %d vertices" %
              (pack, decals_name, len(dat), len(new), added, vertices))


if __name__ == "__main__":
    main(sys.argv[1])
