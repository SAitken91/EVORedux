r"""Colour-code the DLC items in the garage, as on the Xbox 360 (GitHub issue #10).

On the 360, an Origin of Pain item sits on a red tile and a Riders of Doom item on a green one, both
with a small orange "+" in the corner. The PC garage menu has no DLC tile support (its items carry
no pack field), so the tile is painted into the item icons themselves: every garage icon the pack's
own item files list (rider gear and bike parts) gets the pack colour behind its transparent parts and
the "+" badge in the top-left corner. Only the packs' own icon files change; base game icons don't.

Icons are 256 x 256 DXT5 (.png.tex in data_misc.pak: 29-byte T5X header, then the blocks, little-endian
as on PC, then one more byte), the same as the PC's own garage icons.

Usage: python dlc_icon_tiles.py GAME_DIR    (rebuilds from data_misc.pak.before-icon-tiles once made)
"""
import os
import re
import shutil
import sys
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data                    # noqa: E402
from dlc_unpack import names_of                        # noqa: E402
from dlc_convert import write_pak                      # noqa: E402

PACKS = (('OriginofPain', 'pack1', (186, 92, 84)),     # 360 tile colours, sampled from issue #10
         ('RidersofDoom', 'pack2', (64, 140, 60)))
BADGE, PLUS = (240, 160, 30), (60, 30, 0)
SIZE, HEADER = 256, 29


def decode(tex):
    """DXT5 icon -> (256, 256, 4) uint8 RGBA."""
    blocks = np.frombuffer(tex[HEADER:HEADER + SIZE * SIZE], np.uint8).reshape(-1, 16)
    a0, a1 = blocks[:, 0].astype(np.int32), blocks[:, 1].astype(np.int32)
    abits = sum(blocks[:, 2 + k].astype(np.uint64) << np.uint64(8 * k) for k in range(6))
    alut = np.where(a0[:, None] > a1[:, None],
                    np.stack([a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)], 1),
                    np.stack([a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)]
                             + [np.zeros_like(a0), np.full_like(a0, 255)], 1))
    aidx = np.stack([(abits >> np.uint64(3 * k)) & np.uint64(7) for k in range(16)], 1).astype(np.int64)
    alpha = np.take_along_axis(alut, aidx, 1)
    c0 = blocks[:, 8].astype(np.int32) | blocks[:, 9].astype(np.int32) << 8
    c1 = blocks[:, 10].astype(np.int32) | blocks[:, 11].astype(np.int32) << 8
    rgb = lambda c: np.stack([(c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31], 1)
    p0, p1 = rgb(c0), rgb(c1)
    lut = np.stack([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3], 1)
    cbits = blocks[:, 12:16].astype(np.uint32)
    cbits = cbits[:, 0] | cbits[:, 1] << 8 | cbits[:, 2] << 16 | cbits[:, 3] << 24
    cidx = np.stack([(cbits >> (2 * k)) & 3 for k in range(16)], 1).astype(np.int64)
    color = np.take_along_axis(lut, cidx[:, :, None], 1)
    px = np.concatenate([color, alpha[:, :, None]], 2).astype(np.uint8)          # (blocks, 16, 4)
    n = SIZE // 4
    return px.reshape(n, n, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(SIZE, SIZE, 4)


def encode(img):
    """(256, 256, 4) RGBA -> DXT5 icon blocks. Opaque input."""
    n = SIZE // 4
    px = img.reshape(n, 4, n, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4).astype(np.float64)
    out = np.zeros((px.shape[0], 16), np.uint8)
    out[:, 0], out[:, 1] = 255, 255                    # alpha 255 everywhere (every tile is opaque)
    rgb = px[:, :, :3]
    mean = rgb.mean(1, keepdims=True)
    cov = np.einsum('bki,bkj->bij', rgb - mean, rgb - mean)
    axis = np.linalg.eigh(cov)[1][:, :, -1]                                        # principal axis
    proj = np.einsum('bki,bi->bk', rgb - mean, axis)
    lo = mean[:, 0] + axis * proj.min(1, keepdims=True)
    hi = mean[:, 0] + axis * proj.max(1, keepdims=True)
    q = lambda c: (np.clip(np.round(c[:, 0] * 31 / 255), 0, 31).astype(np.int32) << 11 |
                   np.clip(np.round(c[:, 1] * 63 / 255), 0, 63).astype(np.int32) << 5 |
                   np.clip(np.round(c[:, 2] * 31 / 255), 0, 31).astype(np.int32))
    c0, c1 = q(hi), q(lo)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    same = c0 == c1                                     # flat block: 4-colour mode needs c0 > c1
    c1 = np.where(same & (c0 > 0), c0 - 1, c1)
    c0 = np.where(same & (c0 == 0), 1, c0)
    rgbq = lambda c: np.stack([(c >> 11 & 31) * 255 / 31, (c >> 5 & 63) * 255 / 63, (c & 31) * 255 / 31], 1)
    p0, p1 = rgbq(c0), rgbq(c1)
    lut = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)              # (b, 4, 3)
    idx = ((rgb[:, :, None, :] - lut[:, None, :, :]) ** 2).sum(3).argmin(2)       # (b, 16)
    bits = sum(idx[:, k].astype(np.uint32) << (2 * k) for k in range(16))
    out[:, 8], out[:, 9] = c0 & 255, c0 >> 8
    out[:, 10], out[:, 11] = c1 & 255, c1 >> 8
    for k in range(4):
        out[:, 12 + k] = (bits >> (8 * k)) & 255
    return out.tobytes()


def badge(img):
    """Orange "+" badge in the top-left corner, about the size of the 360's."""
    x0, y0, s = 10, 10, 46
    img[y0:y0 + s, x0:x0 + s, :3] = BADGE
    img[y0:y0 + s, x0:x0 + 3, :3] = PLUS                # outline
    img[y0:y0 + s, x0 + s - 3:x0 + s, :3] = PLUS
    img[y0:y0 + 3, x0:x0 + s, :3] = PLUS
    img[y0 + s - 3:y0 + s, x0:x0 + s, :3] = PLUS
    m, t = x0 + s // 2, 4                               # the plus
    img[y0 + 10:y0 + s - 10, m - t:m + t, :3] = PLUS
    img[m - t:m + t, x0 + 10:x0 + s - 10, :3] = PLUS


def tile(tex, colour):
    img = decode(tex).astype(np.float64)
    a = img[:, :, 3:4] / 255
    img[:, :, :3] = img[:, :, :3] * a + np.array(colour) * (1 - a)
    img[:, :, 3] = 255
    img = np.clip(np.round(img), 0, 255).astype(np.uint8)
    badge(img)
    return tex[:HEADER] + encode(img) + tex[HEADER + SIZE * SIZE:]


def pack_icons(configs, base_icons):
    """Icon paths (as in data_misc.pak) of the items the pack's own item files list. Left out: icons
    the PC base game also ships (Riders of Doom's bike reuses the Motocross part icons; its astronaut,
    Tron and top hat icons are the PC's Gold Edition rewards) and the empty-slot icons."""
    d, entries = read_pak(configs)
    icons = set()
    for name, entry in names_of(d, entries):
        if re.fullmatch(r'pack\d+/items\w*\.xml', name):
            xml = file_data(d, entry, decode_xmem=True).decode('utf-8-sig', 'replace')
            for icon in re.findall(r'<item [^>]*\bicon="([^"]+)"', re.sub(r'<!--.*?-->', '', xml, flags=re.S)):
                path = 'images/garage/' + icon.replace('\\', '/').lower() + '.tex'
                if path not in base_icons and '_empty.' not in path:
                    icons.add(path)
    return icons


def install(game):
    done = []
    bd, be = read_pak(os.path.join(game, 'build', 'data', 'data_misc.pak'))
    base_icons = {n.lower() for n, _ in names_of(bd, be)}
    for folder, _, colour in PACKS:
        data = os.path.join(game, 'datapack', 'DLCContent', folder, 'data')
        pak, configs = os.path.join(data, 'data_misc.pak'), os.path.join(data, 'configs.pak')
        if not (os.path.exists(pak) and os.path.exists(configs)):
            continue
        backup, temp = pak + '.before-icon-tiles', pak + '.icon-tiles.tmp'
        if not os.path.exists(backup):
            shutil.copy2(pak, backup)
        icons = pack_icons(configs, base_icons)
        d, entries = read_pak(backup)                  # always from the untouched icons
        names = {id(e): n for n, e in names_of(d, entries)}
        new = {}
        for e in entries:
            name = names.get(id(e), '').lower()
            if name in icons:
                tex = file_data(d, e, decode_xmem=True)
                if len(tex) == HEADER + SIZE * SIZE + 1:
                    new[id(e)] = tile(tex, colour)
        write_pak(temp, [(e['hash'], 8, zlib.compress(new[id(e)], 9), len(new[id(e)])) if id(e) in new else
                         (e['hash'], e['method'], d[e['offset']:e['offset'] + e['packed']], e['size'])
                         for e in entries])
        d2, entries2 = read_pak(temp)                  # verify: only the icons changed
        assert len(entries2) == len(entries)
        for old, now in zip(entries, entries2):
            if id(old) in new:
                assert file_data(d2, now, decode_xmem=True) == new[id(old)]
            else:
                assert d[old['offset']:old['offset'] + old['packed']] == d2[now['offset']:now['offset'] + now['packed']]
        os.replace(temp, pak)
        print(f'{folder}: {len(new)} garage icons on DLC tiles')
        done.append((folder, len(new)))
    return done


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    install(sys.argv[1])
