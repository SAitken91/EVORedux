r"""Give Origin of Pain's Desert Gear helmet (RIDER_PART_HELMET_AFRICAINDY) texture coordinates on its
distance model.

The helmet model has two levels of detail. In the shipped data the far one ("Default-lod1") has u = v = 0
on all 392 vertices, so in a race, where the rider is small enough for that model, every pixel samples one
texel and the hat shows as a flat tan blob (GitHub issue #2); the garage close-up uses the detailed model
and looks right. The far model gets texture coordinates from the detailed one, a triangle at a time: the
detailed model's texture is laid out in separate pieces (UV islands), each far triangle is given the piece
under its centre, and its corners take the coordinates of the closest points on that piece. A far vertex
shared by triangles on different pieces is duplicated, one copy per piece. (v1.5.7 mapped each vertex on
its own; a triangle whose corners landed on different pieces then stretched across the texture and
showed yellow stripes, GitHub issue #2, so those far levels are rebuilt.)

Mesh layout (.mdl, same on Xbox and PC): per level, u32 5bd7085d, u16 vertex count, u32 vertex bytes,
28-byte vertices (f16 position xyz, normal xyz, uv, bone weight, five more f16), then u32 triangle count
and u16 indices. Nothing in the file points into a level, so a level may grow.

Usage: python dlc_lod_uvs.py GAME_DIR    (patches the installed Origin of Pain data.pak; backup kept once)
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

MODELS = ('objects/dlc1/characters/helmets/africaindy/helmet_africaindy_1010057622.mdl',)
LEVEL = b'\x5d\x08\xd7\x5b'
STRIDE, UV = 28, 6                                     # uv = f16 components 6 and 7


def levels(mdl):
    """[(vertex offset, vertices (n x 14 f16), triangles (m x 3))] per level of detail."""
    out, at = [], mdl.find(LEVEL)
    while at >= 0:
        count, size = struct.unpack_from('<HI', mdl, at + 4)
        assert size == count * STRIDE
        start = at + 10
        verts = np.frombuffer(mdl, '<f2', count * 14, start).reshape(count, 14).astype(np.float64)
        tris, = struct.unpack_from('<I', mdl, start + size)
        idx = np.frombuffer(mdl, '<u2', tris * 3, start + size + 4).reshape(tris, 3)
        assert idx.max() < count
        out.append((start, verts, idx))
        at = mdl.find(LEVEL, start + size)
    return out


def closest_uv(p, n, verts, tris):
    """(uv, distance) at the closest point to p on triangles whose normal agrees with n."""
    a, b, c = (verts[tris[:, k], 0:3] for k in range(3))
    tn = np.cross(b - a, c - a)
    facing = (tn @ n) > 0
    ab, ac, ap = b - a, c - a, p - a
    d00, d01, d11 = (ab * ab).sum(1), (ab * ac).sum(1), (ac * ac).sum(1)
    d20, d21 = (ap * ab).sum(1), (ap * ac).sum(1)
    den = np.where(np.abs(d00 * d11 - d01 * d01) < 1e-20, 1e-20, d00 * d11 - d01 * d01)
    v = np.clip((d11 * d20 - d01 * d21) / den, 0, 1)
    w = np.clip((d00 * d21 - d01 * d20) / den, 0, 1)
    s = v + w
    with np.errstate(divide="ignore", invalid="ignore"):
        v, w = np.where(s > 1, v / s, v), np.where(s > 1, w / s, w)
    q = a + ab * v[:, None] + ac * w[:, None]
    dist = ((q - p) ** 2).sum(1) + np.where(facing, 0, 1e3)
    i = dist.argmin()
    uv = verts[tris[i]][:, UV:UV + 2]
    return uv[0] * (1 - v[i] - w[i]) + uv[1] * v[i] + uv[2] * w[i], dist[i]


def islands(tris, count):
    """Island number per triangle: triangles sharing a vertex are on the same texture piece."""
    parent = list(range(count))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for a, b, c in tris:
        ra, rb, rc = find(a), find(b), find(c)
        parent[rb] = ra
        parent[rc] = ra
    return np.array([find(t[0]) for t in tris])


def stretched(verts, tris, factor=8):
    """Triangles whose texture area per surface area is factor times the level's median."""
    p, uv = verts[:, 0:3], verts[:, UV:UV + 2]
    area = np.linalg.norm(np.cross(p[tris[:, 1]] - p[tris[:, 0]], p[tris[:, 2]] - p[tris[:, 0]]), axis=1)
    e1, e2 = uv[tris[:, 1]] - uv[tris[:, 0]], uv[tris[:, 2]] - uv[tris[:, 0]]
    uv_area = np.abs(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0])
    density = uv_area / np.maximum(area, 1e-12)
    return int((density > np.median(density[area > 0]) * factor).sum())


def density(p, uv, tri):
    area = np.linalg.norm(np.cross(p[tri[1]] - p[tri[0]], p[tri[2]] - p[tri[0]]))
    e1, e2 = uv[1] - uv[0], uv[2] - uv[0]
    return abs(e1[0] * e2[1] - e1[1] * e2[0]) / max(area, 1e-12)


def affine_uv(points, ref, tri):
    """uv of each point under the texture mapping of one reference triangle, extended past its edges
    (points are projected onto the triangle's plane)."""
    a, b, c = ref[tri, 0:3]
    ab, ac = b - a, c - a
    d00, d01, d11 = ab @ ab, ab @ ac, ac @ ac
    den = d00 * d11 - d01 * d01 or 1e-20
    uv = ref[tri, UV:UV + 2]
    out = []
    for p in points:
        ap = p - a
        d20, d21 = ap @ ab, ap @ ac
        v, w = (d11 * d20 - d01 * d21) / den, (d00 * d21 - d01 * d20) / den
        out.append(uv[0] * (1 - v - w) + uv[1] * v + uv[2] * w)
    return np.array(out)


def remap_level(verts, tris, ref, ref_tris):
    """(new vertices, new triangles, source vertex of each new vertex): each triangle mapped onto one
    piece of the reference texture.
    The corners take the closest points on the island under the triangle's centre; where an island
    wraps round (a seam inside it) and that would still stretch the triangle, all three corners use
    the mapping of the reference triangle closest to the centre instead."""
    ref_island = islands(ref_tris, len(ref))
    by_island = {i: ref_tris[ref_island == i] for i in np.unique(ref_island)}
    rp = ref[:, 0:3]
    limit = 4 * np.median([density(rp, ref[t, UV:UV + 2], t) for t in ref_tris])
    p, nrm = verts[:, 0:3], verts[:, 3:6]
    new_verts, new_tris, copies, sources = [], [], {}, []
    for n, tri in enumerate(tris):
        centre = p[tri].mean(0)
        normal = np.cross(p[tri[1]] - p[tri[0]], p[tri[2]] - p[tri[0]])
        if not normal.any():
            normal = nrm[tri].sum(0)
        best, island = None, None
        for i, members in by_island.items():
            _, dist = closest_uv(centre, normal, ref, members)
            if best is None or dist < best:
                best, island = dist, i
        uvs = np.array([closest_uv(p[k], nrm[k], ref, by_island[island])[0] for k in tri])
        tag = int(island)
        if density(p, uvs, tri) > limit:                 # crosses a seam inside the island
            members = by_island[island]
            near = min(range(len(members)), key=lambda j: ((rp[members[j]].mean(0) - centre) ** 2).sum())
            uvs = affine_uv(p[tri], ref, members[near])
            tag = ('tri', n)
        out = []
        for k, uv in zip(tri, uvs):
            key = (int(k), tag)
            if key not in copies:
                row = verts[k].copy()
                row[UV:UV + 2] = uv
                copies[key] = len(new_verts)
                new_verts.append(row)
                sources.append(int(k))
            out.append(copies[key])
        new_tris.append(out)
    return np.array(new_verts), np.array(new_tris, dtype=np.uint16), sources


def fix_model(mdl):
    """Model bytes with far levels given texture coordinates (none yet, or stretched ones); None if
    nothing to do."""
    lv = levels(mdl)
    if len(lv) < 2 or np.abs(lv[0][1][:, UV:UV + 2]).max() == 0:
        return None
    ref, ref_tris = lv[0][1], lv[0][2]
    out, changed = bytearray(mdl), False
    for start, verts, tris in reversed(lv[1:]):           # last first: earlier offsets stay valid
        if np.abs(verts[:, UV:UV + 2]).max() != 0 and stretched(verts, tris) == 0:
            continue
        new_verts, new_tris, sources = remap_level(verts, tris, ref, ref_tris)
        assert len(new_verts) < 65536
        raw = bytearray(mdl[start:start + len(verts) * STRIDE])
        rows = []
        for row, src in zip(new_verts, sources):          # keep every field but the uv bit-exact
            data = bytearray(raw[src * STRIDE:(src + 1) * STRIDE])
            struct.pack_into('<2e', data, UV * 2, *row[UV:UV + 2])
            rows.append(bytes(data))
        level = (struct.pack('<HI', len(new_verts), len(new_verts) * STRIDE) + b''.join(rows) +
                 struct.pack('<I', len(new_tris)) + new_tris.astype('<u2').tobytes())
        old_end = start + len(verts) * STRIDE + 4 + len(tris) * 6
        out[start - 6:old_end] = level
        changed = True
    if not changed:
        return None
    check = levels(bytes(out))
    assert len(check) == len(lv)
    assert all(np.abs(v[:, UV:UV + 2]).max() > 0 and stretched(v, t) == 0 for _, v, t in check[1:])
    return bytes(out)


def install(game):
    pak = os.path.join(game, 'datapack', 'DLCContent', 'OriginofPain', 'data', 'data.pak')
    if not os.path.exists(pak):
        return []
    d, entries = read_pak(pak)
    names = {id(e): n for n, e in names_of(d, entries)}
    fixed = {}
    for e in entries:
        if names.get(id(e)) in MODELS:
            new = fix_model(file_data(d, e, decode_xmem=True))
            if new:
                fixed[id(e)] = new
    if not fixed:
        print('helmet distance models: already have texture coordinates')
        return []
    backup, temp = pak + '.before-lod-uvs', pak + '.lod-uvs.tmp'
    if not os.path.exists(backup):
        shutil.copy2(pak, backup)
    write_pak(temp, [(e['hash'], 8, zlib.compress(fixed[id(e)], 9), len(fixed[id(e)])) if id(e) in fixed else
                     (e['hash'], e['method'], d[e['offset']:e['offset'] + e['packed']], e['size'])
                     for e in entries])
    d2, entries2 = read_pak(temp)                      # verify: only the fixed models changed
    assert len(entries2) == len(entries)
    for old, now in zip(entries, entries2):
        if id(old) in fixed:
            assert file_data(d2, now, decode_xmem=True) == fixed[id(old)]
        else:
            assert d[old['offset']:old['offset'] + old['packed']] == d2[now['offset']:now['offset'] + now['packed']]
    os.replace(temp, pak)
    done = [names[i] for i in (id(e) for e in entries) if i in fixed]
    print('helmet distance models given texture coordinates: ' + ', '.join(n.split('/')[-1] for n in done))
    return done


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    install(sys.argv[1])
