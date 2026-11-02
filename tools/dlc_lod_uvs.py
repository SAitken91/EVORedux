r"""Give Origin of Pain's Desert Gear helmet (RIDER_PART_HELMET_AFRICAINDY) texture coordinates on its
distance model.

The helmet model has two levels of detail. In the shipped data the far one ("Default-lod1") has u = v = 0
on all 392 vertices, so in a race, where the rider is small enough for that model, every pixel samples one
texel and the hat shows as a flat tan blob (GitHub issue #2); the garage close-up uses the detailed model
and looks right. Each far-model vertex gets the coordinates of the closest point on the detailed model
(nearest triangle, barycentric, among triangles facing the same way); nothing else in the file changes.

Mesh layout (.mdl, same on Xbox and PC): per level, u32 5bd7085d, u16 vertex count, u32 vertex bytes,
28-byte vertices (f16 position xyz, normal xyz, uv, bone weight, five more f16), then u32 triangle count
and u16 indices.

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
    """uv at the closest point to p on triangles whose normal agrees with n."""
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
    return uv[0] * (1 - v[i] - w[i]) + uv[1] * v[i] + uv[2] * w[i]


def fix_model(mdl):
    """Model bytes with texture coordinates on any far level that has none; None if nothing to do."""
    lv = levels(mdl)
    if len(lv) < 2 or np.abs(lv[0][1][:, UV:UV + 2]).max() == 0:
        return None
    out = bytearray(mdl)
    changed = False
    for start, verts, _ in lv[1:]:
        if np.abs(verts[:, UV:UV + 2]).max() != 0:
            continue
        ref, tris = lv[0][1], lv[0][2]
        for k, vert in enumerate(verts):
            uv = closest_uv(vert[0:3], vert[3:6], ref, tris)
            struct.pack_into('<2e', out, start + k * STRIDE + UV * 2, *uv)
        changed = True
    if not changed:
        return None
    check = levels(bytes(out))
    assert all(np.abs(v[:, UV:UV + 2]).max() > 0 for _, v, _ in check)
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
