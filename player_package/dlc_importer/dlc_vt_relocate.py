r"""Move Origin of Pain's object texture pages out of the PC base game's object VT area.

The object virtual texture is 2048 x 2048 pages at its finest level (11); a page id is
level << 24 | y << 12 | x. The Xbox base game uses columns 0-127 there, and Origin of Pain was
baked into the free columns after it (128-319). The PC base game is laid out wider (columns 0-255),
so 33,701 Origin of Pain pages replaced PC base pages: in Paine Island worlds base objects showed
DLC textures, and on base tracks (where the base page wins) Origin of Pain objects showed base ones.
Riders of Doom (columns 256-511, rows 512-767) overlaps Origin of Pain's columns 256-319.

Only columns 0-1023 of rows 512-1023 are usable: the game never makes a page resident at column
1024 or beyond, or below row 512 (the indirection map shows level 0 there in every capture), so an
earlier +1024 shift left Buccaneer Bay's palms pale and blocky. Origin of Pain is shifted right by
512 columns at level 11 (512 >> (11 - level) at coarser levels, which keeps every page aligned down
to level 2) into columns 640-831. dlc_pc_gear.py puts the PC-only Gold Edition gear aliases at
+768 columns (863-960), so the two only share one level-2 and one level-3 page. Moved:
  - its staged page index (dlc_object_index.py, built fresh from the pack each time),
  - the texture rectangle of each of its materials (first 16 bytes of bakedmeshes1/<id>.raw:
    float add.x, add.y, mul.x, mul.y; add.x += 0.25) whose rectangle starts in columns 128-511.
  - the same rectangles in the pack's material coordinate table (objectcollection1.xml, the table
    dlc_pc_gear.py edits), which ordinary models such as Buccaneer Bay's palm trees read.
    Rectangles already moved (column >= 640) are left alone, so re-running changes nothing.
Nine Origin of Pain materials point at column 48, inside the Xbox base layout; they are untouched.
The object decal table both packs ship is the Xbox base game's (no cell in Origin of Pain's
columns), so it needs no move; dlc_object_index.py still stages it unchanged.

Usage: python dlc_vt_relocate.py SOURCE_PAK OUTPUT_PAK   (Origin of Pain's data/data.pak)
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data                    # noqa: E402
from dlc_unpack import names_of                        # noqa: E402
from dlc_convert import write_pak                      # noqa: E402

SHIFT = {'OriginofPain': 512}            # level-11 columns, per pack folder
OWN = (128, 512)                        # level-11 columns of Origin of Pain's own pages (unshifted)
LEVELS = 11


def shift_page(page, shift):
    level = page >> 24
    if level < 2 or not shift:
        return page
    return page + (shift >> (LEVELS - level))


def shift_rows(rows, shift):
    """Index rows (page, offset, size, file) with their page ids moved."""
    return sorted((shift_page(p, shift), o, s, f) for p, o, s, f in rows)


COLLECTION = 'objectcollection1.xml'


def shift_collection(raw, shift):
    """Material coordinate table with the pack's own rectangles moved; (bytes, count)."""
    from dlc_pc_gear import uv_nodes, rect, set_x, load
    _, nodes = uv_nodes(load(raw))
    out, moved = bytearray(raw), 0
    for node in nodes.values():
        x = rect(node)[0]
        if OWN[0] <= x * 2048 < OWN[1]:
            set_x(out, node, x + shift / 2048)
            moved += 1
    load(bytes(out))
    return bytes(out), moved


def main(source, output, shift=SHIFT['OriginofPain']):
    from dlc_pc_gear import collection
    d, files = read_pak(source)
    names = dict((id(f), n) for n, f in names_of(d, files))
    entries, changed = [], []
    for f in files:
        name = names.get(id(f), '')
        raw = d[f['offset']:f['offset'] + f['packed']]
        if name == COLLECTION:
            xml, _ = collection(file_data(d, f, decode_xmem=True))
            new, moved = shift_collection(xml, shift)
            if moved:
                changed.append(name)
                print('%s: %d material coordinates moved' % (name, moved))
                entries.append((f['hash'], 8, zlib.compress(new, 9), len(new)))
                continue
        if name.startswith('bakedmeshes') and name.endswith('.raw'):
            body = bytearray(file_data(d, f, decode_xmem=True))
            add_x, = struct.unpack_from('<f', body, 0)
            column = add_x * 2048
            if OWN[0] <= column < OWN[1]:
                struct.pack_into('<f', body, 0, add_x + shift / 2048)
                changed.append(name)
                entries.append((f['hash'], 8, zlib.compress(bytes(body), 9), len(body)))
                continue
        entries.append((f['hash'], f['method'], raw, f['size']))
    if not changed:
        print('%s: texture rectangles already relocated' % source)
        return False
    write_pak(output, entries)
    d2, files2 = read_pak(output)                      # verify: only the moved rectangles changed
    assert len(files2) == len(files)
    names2 = dict((id(g), n) for n, g in names_of(d2, files2))
    for f, g in zip(files, files2):
        if names2.get(id(g)) == COLLECTION and COLLECTION in changed:
            continue                                   # verified by shift_collection's reload
        if names2.get(id(g)) in changed:
            a, b = file_data(d, f, decode_xmem=True), file_data(d2, g)
            assert a[4:] == b[4:] and struct.unpack_from('<f', b, 0)[0] * 2048 >= OWN[0] + shift
        else:
            assert d[f['offset']:f['offset'] + f['packed']] == d2[g['offset']:g['offset'] + g['packed']]
    print('%s: %d entries with texture rectangles moved +%d columns' % (output, len(changed), shift))
    return True


def relocate_installed(game):
    """Relocate the installed Origin of Pain data.pak in place (backed up once). True if changed."""
    import shutil
    pak = os.path.join(game, 'datapack', 'DLCContent', 'OriginofPain', 'data', 'data.pak')
    if not os.path.exists(pak):
        return False
    backup, temp = pak + '.before-vt-relocate', pak + '.vt-relocate.tmp'
    if not os.path.exists(backup):
        shutil.copy2(pak, backup)
    try:
        changed = main(pak, temp)
        if changed:
            os.replace(temp, pak)
        return changed
    finally:
        if os.path.exists(temp):
            os.remove(temp)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
