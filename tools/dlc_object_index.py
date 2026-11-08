r"""Stage per-world PC+DLC object VT indices and noncolliding raw filenames.

Also stages each pack's object decal table (vtex/decalsObjects.dat from its data.pak, already
converted by dlc_vtex.py) as build\data\vtex\decalsObjects_dlc<world>.dat: the game hardcodes the
base table for every world, and the client shim switches DLC worlds to these (patchy seabed on
Buccaneer Bay and other Paine Island tracks without them). The packs ship the Xbox base table, whose
cells paint the Xbox object layout; used whole, it repainted base objects in DLC worlds (the
Scorpion's rear fender turned red). So the staged table is the PC base table, with the pack's cells
only where DLC materials use base-area pages (the Origin of Pain seabed, columns 48-160), except
cells that also hold PC bike or rider textures: there the pack's Xbox decals dirtied the bike engines
and greyed the Micro Donkey in DLC worlds (GitHub issue #2), so those 12 cells keep the PC decals.
(A decal's position is not a plain atlas position, so cells are kept or replaced whole.) Once a
pack is loaded, vtex/decalsObjects.dat resolves to its copy in base worlds too, so the PC table is
also staged as decalsObjects_dlc0.dat for them.

Uses the installed archives (not potentially stale conversion output). Original
base index and raw files are never changed. Pair with the object-index shim hook.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zlib

from pak import read_pak, file_data
from dlc_events import name_hash
from dlc_vtex import raw_file_name
from dlc_vt_relocate import SHIFT as RELOCATE, shift_rows


def rows(data):
    count, = struct.unpack_from('<I', data)
    assert len(data) == 4 + count * 11
    result = list(struct.iter_unpack('<IIHB', data[4:]))
    assert len({r[0] for r in result}) == count
    assert result == sorted(result)
    return result


DECAL_CELL = 16          # level-11 pages per object decal cell (header: 0x800 pages, 0x80 cells)


def decal_cells(dat):
    """Object decal table -> (header, raw cells: b'\x00' or b'\x01' + u32 size + data)."""
    assert struct.unpack_from('<II', dat) == (0x800, 0x80)
    cells, o = [], 8
    while o < len(dat):
        if dat[o] == 0:
            cells.append(dat[o:o + 1])
            o += 1
            continue
        assert dat[o] == 1
        size, = struct.unpack_from('<I', dat, o + 1)
        cells.append(dat[o:o + 5 + size])
        o += 5 + size
    return dat[:8], cells


def base_area_cells(game):
    """Decal cells (x, y) under DLC material rectangles in the base object columns (< 256)."""
    from dlc_unpack import names_of
    from dlc_pc_gear import collection, uv_nodes, rect, load
    cells = set()
    for pack in ('OriginofPain', 'RidersofDoom'):
        pak = game / 'datapack/DLCContent' / pack / 'data/data.pak'
        if not pak.exists():
            continue
        data, entries = read_pak(str(pak))
        rects = []
        for name, entry in names_of(data, entries):
            if name.startswith('objectcollection'):
                tree, _ = collection(file_data(data, entry, decode_xmem=True))
                rects += [rect(n) for n in uv_nodes(load(tree))[1].values()]
            elif name.startswith('bakedmeshes'):
                head = file_data(data, entry, decode_xmem=True)[:16]
                if len(head) == 16:
                    rects.append(struct.unpack('<4f', head))
        for x, y, w, h in rects:
            if not (0 <= x <= 1 and 0 <= y <= 1 and w > 0 and h > 0) or x * 2048 >= 256:
                continue
            x0, y0 = int(x * 2048) // DECAL_CELL, int(y * 2048) // DECAL_CELL
            x1, y1 = int((x + w) * 2048 - 1) // DECAL_CELL, int((y + h) * 2048 - 1) // DECAL_CELL
            cells.update((cx, cy) for cy in range(y0, y1 + 1) for cx in range(x0, x1 + 1))
    return cells


def bike_cells(game):
    """Decal cells (x, y) holding a PC bike or rider material (Objects/Bikes, Objects/Characters)."""
    from dlc_unpack import names_of
    from dlc_pc_gear import collection, uv_nodes, rect, load
    from dlc_fire_materials import walk
    data, entries = read_pak(str(game / 'build/data/data.pak'))
    cells = set()
    for name, entry in names_of(data, entries):
        if not name.startswith('objectcollection'):
            continue
        tree, _ = collection(file_data(data, entry, decode_xmem=True))
        root = load(tree)
        _, nodes = uv_nodes(root)
        for node, path in walk(root):
            if 'Materials' not in path or not ('Bikes' in path or 'Characters' in path):
                continue
            props = {key: value for key, _kind, value in node['props']}
            if 0x255b12 not in props:
                continue
            material = struct.unpack('<I', props[0x255b12])[0]
            if material not in nodes:
                continue
            x, y, w, h = (v * 2048 for v in rect(nodes[material]))
            if w > 0 and h > 0:
                cells.update((cx, cy) for cy in range(int(y) // DECAL_CELL, int(y + h - 1) // DECAL_CELL + 1)
                             for cx in range(int(x) // DECAL_CELL, int(x + w - 1) // DECAL_CELL + 1))
    return cells


def merge_decals(pc_dat, pack_dat, keep):
    """PC object decal table with the pack's cells at `keep`; (bytes, cells taken from the pack)."""
    head, pc = decal_cells(pc_dat)
    _, pack = decal_cells(pack_dat)
    assert len(pc) == len(pack)
    out, kept = [], 0
    for i, cell in enumerate(pc):
        if (i % 0x80, i // 0x80) in keep and pack[i] != cell:
            cell = pack[i]
            kept += 1
        out.append(cell)
    return head + b''.join(out), kept


def base_material_rects(game):
    """Level-11 page rectangles (x0, y0, x1, y1) of every PC base-game material."""
    from dlc_unpack import names_of
    from dlc_pc_gear import collection, uv_nodes, rect, load
    data, entries = read_pak(str(game / 'build/data/data.pak'))
    out = []
    for name, entry in names_of(data, entries):
        if name.startswith('objectcollection'):
            tree, _ = collection(file_data(data, entry, decode_xmem=True))
            for node in uv_nodes(load(tree))[1].values():
                x, y, w, h = (v * 2048 for v in rect(node))
                if w > 0 and h > 0:
                    out.append((x, y, x + w, y + h))
    return out


def baked_strip_pages(data, entries, original, shift, base_rects):
    """Origin of Pain pages to keep at their original ids as well as moved ones.

    Nine baked track meshes (bakedmeshes1, e.g. the circus cargo containers) have texture rectangles
    starting at column 48, in the base area, and reaching into Origin of Pain's own columns. They
    stay put (their base-area part is painted by the object decal table), so the part past column
    128 must still find Origin of Pain's pages at the original ids; moved alone, those spots showed
    PC base pages (white containers with stray paint, #2). Pages under any PC base material are left
    to the base game (Motocross hand shields, signs, lights and others share that strip)."""
    from dlc_unpack import names_of
    from dlc_vt_relocate import OWN
    if not shift:
        return []
    strips = []
    for name, entry in names_of(data, entries):
        if name.startswith('bakedmeshes') and name.endswith('.raw'):
            head = file_data(data, entry, decode_xmem=True)[:16]
            if len(head) == 16:
                x, y, w, h = (v * 2048 for v in struct.unpack('<4f', head))
                if x < OWN[0] < x + w:
                    strips.append((OWN[0], y, x + w, y + h))
    keep = []
    for row in original:
        level = row[0] >> 24
        if level < 2:
            continue
        size = 1 << (11 - level)
        x0, y0 = (row[0] & 0xfff) * size, ((row[0] >> 12) & 0xfff) * size
        x1, y1 = x0 + size, y0 + size
        inside = lambda r: x0 < r[2] and r[0] < x1 and y0 < r[3] and r[1] < y1
        if any(inside(r) for r in strips) and not any(inside(r) for r in base_rects):
            keep.append(row)
    return keep


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    base_data = (args.game / 'build/data/vtex/vtex_objects.raw.hdr').read_bytes()
    base = rows(base_data)
    used = {r[3] for r in base}
    report = {'base_sha256': hashlib.sha256(base_data).hexdigest(), 'packs': []}
    next_code = 0x84
    dlc_cells = base_area_cells(args.game) - bike_cells(args.game)
    base_rects = base_material_rects(args.game)
    # Base worlds: the PC table under a name no pack shadows (both ship vtex/decalsObjects.dat).
    pc_decals = (args.game / 'build/data/vtex/decalsObjects.dat').read_bytes()
    decal_cells(pc_decals)
    (args.output / 'decalsObjects_dlc0.dat').write_bytes(pc_decals)
    for world, pack in enumerate(('OriginofPain', 'RidersofDoom'), 1):
        root = args.game / 'datapack/DLCContent' / pack
        if not (root / 'data/data.pak').exists():
            print(f'{pack}: not installed, skipped', flush=True)
            continue
        hdr_name = f'vtex/vtex_objects{world+1}.raw.hdr'
        data, entries = read_pak(str(root / 'data/data.pak'))
        hdr = file_data(data, next(e for e in entries if e['hash'] == name_hash(hdr_name)))
        # Origin of Pain's pages move with its material rectangles (dlc_vt_relocate.py).
        original = rows(hdr)
        dlc = shift_rows(original, RELOCATE.get(pack, 0))
        strip = baked_strip_pages(data, entries, original, RELOCATE.get(pack, 0), base_rects)
        if strip:
            print(f'{pack}: {len(strip)} pages also kept at their original ids for its baked meshes', flush=True)
        dlc = sorted({r[0]: r for r in strip + dlc}.values())
        merged = {r[0]: r for r in base}
        mapping = {}
        files = []
        for code in sorted({r[3] for r in dlc}):
            assert next_code not in used and next_code < 0xfe
            used.add(next_code)
            mapping[code] = next_code
            src = root / 'vtex' / raw_file_name(hdr_name, code, str(root / 'vtex'))
            name = f'vtex_objects_{next_code & 15}_{next_code >> 4}.raw'
            dst = args.output / name
            raw = src.read_bytes()
            blocks = {(r[1] & 0x7fffffff, r[2]) for r in dlc if r[3] == code}
            for offset, size in blocks:
                assert (offset + size) * 4 <= len(raw)
                if size:
                    zlib.decompress(raw[offset*4:(offset+size)*4])
            digest = hashlib.sha256(raw).hexdigest()
            if not dst.exists() or hashlib.sha256(dst.read_bytes()).hexdigest() != digest:
                shutil.copyfile(src, dst)
            files.append({'name': name, 'source': str(src), 'sha256': digest,
                          'blocks_verified': len(blocks), 'size': len(raw)})
            next_code += 1
        for page, offset, size, code in dlc:
            merged[page] = (page, offset, size, mapping[code])
        output = struct.pack('<I', len(merged)) + b''.join(
            struct.pack('<IIHB', *merged[p]) for p in sorted(merged))
        assert len(rows(output)) == len(merged)
        decals = next((e for e in entries if e['hash'] == name_hash('vtex/decalsObjects.dat')), None)
        decals_name = None
        if decals:
            decals_name = f'decalsObjects_dlc{world}.dat'
            table, kept = merge_decals(pc_decals, file_data(data, decals), dlc_cells)
            (args.output / decals_name).write_bytes(table)
            print(f'{pack}: object decals = PC table + {kept} pack cells', flush=True)
        name = f'vtex_objects_dlc{world}.raw.hdr'
        (args.output / name).write_bytes(output)
        item = {'pack': pack, 'world': world, 'decals': decals_name,
                'base_pages': len(base), 'dlc_pages': len(dlc),
                'merged_pages': len(merged), 'overlapping_pages': len(base)+len(dlc)-len(merged),
                'header': name, 'files': files, 'mapping': mapping}
        report['packs'].append(item)
        print(f'{pack}: {len(dlc)} DLC pages, {len(merged)} merged; all compressed blocks verified', flush=True)
    (args.output / 'object_index_manifest.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
