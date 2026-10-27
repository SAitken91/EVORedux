"""Add pack<N>/items_bikes.xml (the DLC bike's garage entries) to a DLC configs.pak.

The PC engine only reads pack0/items.xml (startup, 0x4334e5 -> loader 0x657910 into
[[0x1ee3548]+0x6c]). Without the DLC bike's <bike id="5|6" gfx=...> entry, the garage
(GameStateGarage::init 0x63c440) asks the item table for the bike model (0x656aa0), loads
nothing (0x791420 returns null) and calls through the null object at 0x63d125 as soon as the
bike-select screen opens on a DLC track that offers the DLC bike.
The pack's own items.xml cannot simply be loaded on top: its <characters> section repeats the
base game's character/charpart categories (and reuses item id 301). Its <bikes> and <bikeparts>
sections only use new ids (bike 5/6, part categories 24+, items 4xx), so this writes a copy that
keeps those two sections in the base file's skeleton (see bikes_only); the DLL loads it next to the
pack's bikes.xml.
Usage: python dlc_items.py SOURCE_CONFIGS_PAK OUTPUT_PAK
"""
import pathlib
import re
import struct
import sys
import xml.etree.ElementTree as ET
import zlib
from pak import read_pak, file_data
from dlc_unpack import names_of
from dlc_convert import write_pak
from dlc_events import name_hash, NAME_TABLE


def bikes_only(xml):
    text = xml.decode('utf-8-sig')
    sections = []
    for tag in ('bikes', 'bikeparts'):
        m = re.search(r'<%s>.*?</%s>' % (tag, tag), re.sub(r'<!--.*?-->', '', text, flags=re.S), re.S)
        assert m, tag
        sections.append(m.group(0))
    # The loader (0x657910) walks the document by position with unchecked first-child (0x75b220)
    # / next-sibling (0x75b050) chains: items > characters > character > (charpart, checked);
    # characters ~ bikes > bike; ~ bikeparts > part; ~ mplocalparts > (child, checked). So every
    # section must exist in that order, and characters needs a character (no charparts: skipped).
    out = ('<items>\n\t<characters>\n\t\t<character id="0" name="RIDER_HEAVY_GUY">\n\t\t</character>\n'
           '\t</characters>\n\t' + '\n\t'.join(sections) + '\n\t<mplocalparts>\n\t</mplocalparts>\n</items>\n')
    root = ET.fromstring(out)
    assert [c.tag for c in root] == ['characters', 'bikes', 'bikeparts', 'mplocalparts']
    assert len(root[0]) == 1 and len(root[0][0]) == 0 and len(root[3]) == 0
    bikes = [b.get('id') for b in root.findall('./bikes/bike')]
    assert bikes and all(b.get('gfx') for b in root.findall('./bikes/bike'))
    return out.encode('utf-8'), bikes


def convert(source, destination):
    data, entries = read_pak(source)
    named = names_of(data, entries)
    names = [n for n, _ in named]
    m = [n for n in names if re.fullmatch(r'pack\d+/items\.xml', n)]
    assert len(m) == 1, m
    new_name = m[0].replace('items.xml', 'items_bikes.xml')
    assert new_name not in names, 'Already added; no output written'
    payload, bikes = bikes_only(file_data(data, dict(named)[m[0]], decode_xmem=True))
    print(m[0], '->', new_name, 'bikes', bikes, len(payload), 'bytes')
    output = [(f['hash'], f['method'], data[f['offset']:f['offset'] + f['packed']], f['size']) for _, f in named]
    output.append((name_hash(new_name), 8, zlib.compress(payload, 9), len(payload)))
    names.append(new_name)
    if any(f['hash'] == NAME_TABLE for f in entries):     # name table last and stored, as in the originals
        blob = struct.pack('<I', len(names)) + b''.join(struct.pack('<H', len(n)) + n.encode('latin-1') for n in names)
        output.append((NAME_TABLE, 0, blob, len(blob)))
    write_pak(destination, output)
    result, check = read_pak(destination)
    renamed = dict(names_of(result, check))
    assert list(renamed) == names
    for name, f in named:
        g = renamed[name]
        assert (g['hash'], g['method'], g['size']) == (f['hash'], f['method'], f['size'])
        assert result[g['offset']:g['offset'] + g['packed']] == data[f['offset']:f['offset'] + f['packed']]
    assert file_data(result, renamed[new_name]) == payload
    print('Verified output archive; existing entries unchanged, new entry readable')


if __name__ == '__main__':
    assert pathlib.Path(sys.argv[1]).resolve() != pathlib.Path(sys.argv[2]).resolve()
    convert(sys.argv[1], sys.argv[2])
