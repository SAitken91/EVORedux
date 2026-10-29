"""Keep PC-exclusive rider/bike textures separate from Xbox DLC atlas allocations.

Paine Island's circus materials overlap the Gold Edition bonus gear at the same virtual UVs.
Move only PC_exclusive material rectangles +0.25 in U, into unused object-atlas space, and
alias the original PC pages there. Raw texture pixels, mesh UVs and DLC rectangles stay intact.
Stages complete candidate files first; --install backs up changed destinations once.
Requires the current client shim's per-world/base object-index hooks. Local game assets only.
"""
from pathlib import Path
import argparse
import hashlib
import json
import math
import shutil
import struct
import zlib

from dlc_convert import write_pak
from dlc_fire_materials import load, walk
from dlc_object_index import rows
from dlc_unpack import names_of
from pak import read_pak, file_data

MATERIAL_ID = 0x255b12
ID, X, Y, W, H = 0x36ef52, 0x78, 0x79, 0x7a, 0x77
UV_TABLE = 0x978cb0d6
SHIFT = 0.25
PACKS = (('OriginofPain', 1), ('RidersofDoom', 2))


def properties(node):
    return {key: (kind, value) for key, kind, value in node['props']}


def collection(data):
    if data[:2] != b'\x01\x00':
        from xmem import xmem_decompress
        data = xmem_decompress(data[8:])
    return data, load(data)


def uv_nodes(tree):
    tables = [n for n, _ in walk(tree) if n['h'] == UV_TABLE]
    if len(tables) != 1:
        raise ValueError('Expected one virtual-material coordinate table')
    found = {}
    for n in tables[0]['kids']:
        props = properties(n)
        if all(k in props for k in (ID, X, Y, W, H)):
            mid, = struct.unpack('<I', props[ID][1])
            if mid in found:
                raise ValueError('Duplicate material coordinate id')
            found[mid] = n
    return tables[0], found


def rect(node):
    p = properties(node)
    return tuple(struct.unpack('<f', p[k][1])[0] for k in (X, Y, W, H))


def set_x(data, node, value):
    """Change just the native float property; retain all other binary XML bytes."""
    pos = node['s'] + 6
    while data[pos] == 1:
        key, = struct.unpack_from('<I', data, pos + 2)
        size, = struct.unpack_from('<H', data, pos + 7)
        if key == X:
            if data[pos + 6] != 5 or size != 4:
                raise ValueError('Unsupported material U coordinate')
            data[pos + 9:pos + 13] = struct.pack('<f', value)
            return
        pos += 9 + size
    raise ValueError('Material coordinate lacks U')


def pc_materials(data, tree):
    ids = set()
    for n, path in walk(tree):
        p = properties(n)
        if 'PC_exclusive' in path and 'Materials' in path and MATERIAL_ID in p:
            ids.add(struct.unpack('<I', p[MATERIAL_ID][1])[0])
    _, nodes = uv_nodes(tree)
    selected = {mid: n for mid, n in nodes.items() if mid in ids and rect(n)[2] > 0 and rect(n)[3] > 0}
    if not selected:
        raise ValueError('No PC-exclusive virtual-texture materials found')
    shifted = {rect(n)[0] >= SHIFT for n in selected.values()}
    if len(shifted) != 1:
        raise ValueError('Mixed original/relocated PC material coordinates')
    was_shifted = shifted.pop()
    original = {}
    output = bytearray(data)
    for mid, node in selected.items():
        x, y, w, h = rect(node)
        if was_shifted:
            x -= SHIFT
        if not (0 <= x < x + w <= SHIFT and 0.25 <= y < y + h <= 0.5):
            raise ValueError(f'Unexpected PC object-atlas rectangle for {mid}')
        original[mid] = (x, y, w, h)
        set_x(output, node, x + SHIFT)
    load(bytes(output))
    return bytes(output), selected, original


def alias_pages(base, rectangles):
    aliases = {}
    for page, offset, size, file in base:
        level, py, px = page >> 24, (page >> 12) & 4095, page & 4095
        if not 2 <= level <= 11:
            raise ValueError('Unsupported object-atlas mip level')
        scale = 1 << level
        # Include adjacent source tiles so mip filtering at a rectangle boundary stays valid.
        if any(math.floor(x * scale) - 1 <= px <= math.ceil((x + w) * scale) and
               math.floor(y * scale) - 1 <= py <= math.ceil((y + h) * scale)
               for x, y, w, h in rectangles):
            target = (level << 24) | (py << 12) | (px + (1 << (level - 2)))
            aliases[target] = (target, offset, size, file)
    if not aliases:
        raise ValueError('No source PC pages selected')
    return aliases


def with_aliases(index, aliases):
    merged = {r[0]: r for r in rows(index)}
    for page, row in aliases.items():
        if page in merged and merged[page] != row:
            raise ValueError(f'Relocated PC page conflicts with existing content: {page:08x}')
        merged[page] = row
    result = struct.pack('<I', len(merged)) + b''.join(struct.pack('<IIHB', *merged[k]) for k in sorted(merged))
    rows(result)
    return result


def check_terrain_aliases(index, aliases):
    if any(r[0] in aliases for r in rows(index)):
        raise ValueError('Relocated PC pages conflict with a terrain allocation')


def rewrite_archive(source, target, filename, transform):
    data, entries = read_pak(source)
    named = dict(names_of(data, entries))
    entry = named[filename]
    raw, _ = collection(file_data(data, entry, decode_xmem=True))
    changed = transform(raw)
    load(changed)
    target.parent.mkdir(parents=True, exist_ok=True)
    if raw == changed:
        shutil.copyfile(source, target)
        return
    write_pak(target, [(e['hash'], 8, zlib.compress(changed, 9), len(changed)) if e is entry else
                       (e['hash'], e['method'], data[e['offset']:e['offset'] + e['packed']], e['size']) for e in entries])
    check, after = read_pak(target)
    if len(after) != len(entries):
        raise ValueError('Archive entry count changed')
    for old, new in zip(entries, after):
        if old['hash'] != new['hash']:
            raise ValueError('Archive entry order changed')
        if old is entry:
            if file_data(check, new, decode_xmem=True) != changed:
                raise ValueError('Changed collection did not round-trip')
        elif (old['method'], old['size'], data[old['offset']:old['offset'] + old['packed']]) != (
                new['method'], new['size'], check[new['offset']:new['offset'] + new['packed']]):
            raise ValueError('Unrelated archive entry changed')


def stage(game, output):
    game, output = Path(game), Path(output)
    if game.resolve() == output.resolve():
        raise ValueError('Stage into a separate folder, not the game directory')
    base_path = game / 'build/data/data.pak'
    data, entries = read_pak(base_path)
    named = dict(names_of(data, entries))
    base_xml, base_tree = collection(file_data(data, named['objectcollection.xml'], decode_xmem=True))
    moved, selected, original = pc_materials(base_xml, base_tree)
    vtex = game / 'build/data/vtex'
    base_index = (vtex / 'vtex_objects.raw.hdr').read_bytes()
    aliases = alias_pages(rows(base_index), original.values())
    check_terrain_aliases((vtex / 'vtex_terrain.raw.hdr').read_bytes(), aliases)
    files = []

    def record(relative):
        files.append(str(relative).replace('\\', '/'))
        return output / relative

    relative = Path('build/data/data.pak')
    rewrite_archive(base_path, record(relative), 'objectcollection.xml', lambda b: moved)
    alias_nodes = {}
    _, moved_nodes = uv_nodes(load(moved))
    for mid in selected:
        n = moved_nodes[mid]
        alias_nodes[mid] = moved[n['s']:n['e']]

    def extend_dlc(raw):
        table, existing = uv_nodes(load(raw))
        missing = []
        for mid, blob in sorted(alias_nodes.items()):
            if mid in existing:
                if raw[existing[mid]['s']:existing[mid]['e']] != blob:
                    raise ValueError(f'DLC material {mid} conflicts with PC-only relocation')
            else:
                missing.append(b'\x01' + blob)
        pos = table['e'] - 1
        return raw[:pos] + b''.join(missing) + raw[pos:]

    own_indices = {}
    for folder, world in PACKS:
        relative = Path(f'datapack/DLCContent/{folder}/data/data.pak')
        if not (game / relative).exists():
            continue
        pack_data, pack_entries = read_pak(game / relative)
        for filename, entry in names_of(pack_data, pack_entries):
            if filename.startswith('vtex/vtex_terrain') and filename.endswith('.raw.hdr'):
                check_terrain_aliases(file_data(pack_data, entry, decode_xmem=True), aliases)
        rewrite_archive(game / relative, record(relative), f'objectcollection{world}.xml', extend_dlc)
        relative = Path(f'build/data/vtex/vtex_objects_dlc{world}.raw.hdr')
        index = with_aliases((game / relative).read_bytes(), aliases)
        target = record(relative); target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(index)
        own_indices[world] = {r[0]: r for r in rows(index)}
    if not own_indices:
        raise ValueError('No installed DLC packs found')

    # Regenerate the shim's derived caches too, so an old or future-dated cache cannot win.
    for world in [0, *own_indices]:
        merged = {r[0]: r for r in rows(base_index)} if world == 0 else dict(own_indices[world])
        for other in [1, 2]:
            for page, row in own_indices.get(other, {}).items():
                merged.setdefault(page, row)
        index = struct.pack('<I', len(merged)) + b''.join(struct.pack('<IIHB', *merged[p]) for p in sorted(merged))
        suffix = '0' if world == 0 else f'{world}_all'
        relative = Path(f'build/data/vtex/vtex_objects_dlc{suffix}.raw.hdr')
        record(relative).write_bytes(index)
    manifest = dict(materials=len(selected), aliased_pages=len(aliases), shift_u=SHIFT, files=files,
                    sha256={p: hashlib.sha256((output / p).read_bytes()).hexdigest() for p in files},
                    input_sha256={p: hashlib.sha256((game / p).read_bytes()).hexdigest()
                                  if (game / p).exists() else None for p in files})
    (output / 'pc_gear_manifest.json').write_text(json.dumps(manifest, indent=2))
    return manifest


def install(game, output):
    game, output = Path(game), Path(output)
    manifest = json.loads((output / 'pc_gear_manifest.json').read_text())
    # Validate the complete candidate and freshness before the first installed file changes.
    for relative in manifest['files']:
        source, target = output / relative, game / relative
        if not target.resolve().is_relative_to(game.resolve()) or not source.resolve().is_relative_to(output.resolve()):
            raise ValueError('Manifest path leaves the game or staging folder')
        if hashlib.sha256(source.read_bytes()).hexdigest() != manifest['sha256'][relative]:
            raise ValueError('Staged candidate changed since validation')
        current = hashlib.sha256(target.read_bytes()).hexdigest() if target.exists() else None
        if current not in (manifest['input_sha256'][relative], manifest['sha256'][relative]):
            raise ValueError(f'Installed file changed since staging: {relative}')
    for relative in manifest['files']:
        source, target = output / relative, game / relative
        if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest() == manifest['sha256'][relative]:
            continue
        backup = target.with_name(target.name + '.before-pc-gear')
        if target.exists() and not backup.exists():
            shutil.copy2(target, backup)
        shutil.copy2(source, target)
        if hashlib.sha256(target.read_bytes()).hexdigest() != manifest['sha256'][relative]:
            raise ValueError(f'Installed hash mismatch: {relative}')
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game', type=Path)
    ap.add_argument('output', type=Path)
    ap.add_argument('--install', action='store_true')
    args = ap.parse_args()
    if args.install:
        from dlc_import import game_running
        if game_running():
            raise SystemExit('Close Trials before installing the PC gear repair')
    manifest = stage(args.game, args.output)
    if args.install:
        install(args.game, args.output)
    print(f"{'Installed' if args.install else 'Staged'}: {manifest['materials']} PC materials, "
          f"{manifest['aliased_pages']} texture-page aliases; original raw pixels preserved")


if __name__ == '__main__':
    main()
