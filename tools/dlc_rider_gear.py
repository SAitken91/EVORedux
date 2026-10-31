"""Add the Xbox 360 rider gear the PC port never listed to the garage (GitHub issue #10).

The PC reads only pack0/items.xml for garage items (0x4334e5 -> 0x657910). The DLC packs'
pack<N>/items.xml carry their rider gear in a <characters> section, but loading that file as well
would add a second copy of every gear category (the loader allocates a new category per <charpart>),
so the gear is copied into pack0/items.xml instead, into the base category with the same bindname.
Everything else is already in place: models (pack objectcollections), icons (pack data_misc.pak) and
names (pack localization).

  - Origin of Pain: its helmets, Team 1/2 jerseys and the DLC shorts. Jersey 301 and shorts 302
    collide with the PC-exclusive back rims 301/302, so they become 439/440.
  - Riders of Doom: its helmets and torsos.
  - Base game: the Ultimate Endurance and Trials HD shirts. The PC's character_shirts_1.gfx is
    identical to the 360's (variations ue_shirt / hd_shirt) and both icons ship; only the item
    entries were dropped. They are written here because players have no 360 base game files.
Pieces the PC already has as its own Uplay rewards (astronaut, Tron, ancient armor, top hat) are
left out so they do not appear twice. Items already present (by name) are skipped, so re-running
changes nothing.

Usage: python dlc_rider_gear.py GAME_FOLDER
"""
import os
import re
import shutil
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data                    # noqa: E402
from dlc_unpack import names_of                        # noqa: E402
from dlc_convert import write_pak                      # noqa: E402

ITEMS = 'pack0/items.xml'
PACKS = (('OriginofPain', 'pack1/items.xml'), ('RidersofDoom', 'pack2/items.xml'))
SKIP = {
    'RIDER_PART_HELMET_ASTRONAUT', 'RIDER_PART_HELMET_TRON',              # PC: astronaut/tron helmet
    'RIDER_PART_TORSO_ASTRONAUT', 'RIDER_PART_BOTTOM_ASTRONAUT',
    'RIDER_PART_GLOVES_ASTRONAUT', 'RIDER_PART_BOOTS_ASTRONAUT',          # PC: space suit
    'RIDER_PART_TORSO_TRON', 'RIDER_PART_TORSO_SAURON',                   # PC: cyber suit, ancient armor
    'RIDER_PART_HELMET_TOPHAT',                                           # PC: top hat
}
RENUMBER = {'RIDER_PART_DLC_SHIRT': 439, 'RIDER_PART_DLC_RIDING_SHORTS': 440}
BASE_SHIRTS = (                                                           # from the 360 pack0/items.xml
    '<item id="58" name="RIDER_PART_UE_SHIRT" icon="char\\items\\items_shirt_ue.png" colorizable="0" '
    'price="0" medalreq="0" lisencereq="0" outfitset="8" collectionid="3326671378" variationid="512" '
    'nullcolor="1776312153:5" />',
    '<item id="65" name="RIDER_PART_HD_SHIRT" icon="char\\items\\items_shirt_hd.png" colorizable="1" '
    'price="0" medalreq="0" lisencereq="0" outfitset="9" collectionid="3326671378" variationid="1024" '
    'color="17814826:5" />',
)


def strip_comments(xml):
    return re.sub(r'<!--.*?-->', '', xml, flags=re.S)


def character_items(xml):
    """[(bindname, item tag)] from a <characters> section, comments removed."""
    section = re.search(r'<characters>.*?</characters>', strip_comments(xml), re.S)
    out = []
    if section:
        for part in re.finditer(r'<charpart [^>]*bindname="(\w+)"[^>]*>(.*?)</charpart>', section.group(), re.S):
            out += [(part.group(1), tag) for tag in re.findall(r'<item [^>]*/>', part.group(2))]
    return out


def attr(tag, name):
    m = re.search(r'\b%s="([^"]*)"' % name, tag)
    return m.group(1) if m else None


def add_items(base, extra):
    """base pack0/items.xml text plus the extra (bindname, tag) items; (text, names added)."""
    live = strip_comments(base)
    names = set(re.findall(r'<item [^>]*\bname="([^"]+)"', live))
    ids = {int(i) for i in re.findall(r'<item id="(\d+)"', live)}
    added = []
    for bind, tag in extra:
        name = attr(tag, 'name')
        if name in names or name in SKIP:
            continue
        if name in RENUMBER:
            tag = re.sub(r'\bid="\d+"', 'id="%d"' % RENUMBER[name], tag, count=1)
        item_id = int(attr(tag, 'id'))
        if item_id in ids:
            raise ValueError(f'{name}: item id {item_id} is already used')
        section = re.search(r'<characters>.*?</characters>', base, re.S)
        part = re.search(r'<charpart [^>]*bindname="%s"[^>]*>.*?(\s*)</charpart>' % bind, section.group(), re.S)
        if not part:
            raise ValueError(f'{name}: no {bind} category in {ITEMS}')
        at = section.start() + part.start(1)                 # end of the category's item list
        base = base[:at] + '\n\t\t\t\t\t' + tag + base[at:]
        names.add(name)
        ids.add(item_id)
        added.append(name)
    return base, added


def main(game):
    pak = os.path.join(game, 'build', 'data', 'data.pak')
    d, entries = read_pak(pak)
    named = dict(names_of(d, entries))
    raw = file_data(d, named[ITEMS], decode_xmem=True)
    text = raw.decode('utf-8-sig')
    extra = [('SHIRT', tag) for tag in BASE_SHIRTS]
    for folder, items in PACKS:
        configs = os.path.join(game, 'datapack', 'DLCContent', folder, 'data', 'configs.pak')
        if not os.path.exists(configs):
            continue
        cd, ce = read_pak(configs)
        cn = dict(names_of(cd, ce))
        if items in cn:
            extra += character_items(file_data(cd, cn[items], decode_xmem=True).decode('utf-8-sig'))
    text, added = add_items(text, extra)
    if not added:
        print(f'{ITEMS}: rider gear already listed')
        return []
    new = text.encode('utf-8')
    if raw.startswith(b'\xef\xbb\xbf'):
        new = b'\xef\xbb\xbf' + new
    character_items(new.decode('utf-8-sig'))                # still parses
    backup, temp = pak + '.before-rider-gear', pak + '.rider-gear.tmp'
    if not os.path.exists(backup):
        shutil.copy2(pak, backup)
    target = named[ITEMS]
    write_pak(temp, [(e['hash'], 8, zlib.compress(new, 9), len(new)) if e is target else
                     (e['hash'], e['method'], d[e['offset']:e['offset'] + e['packed']], e['size'])
                     for e in entries])
    d2, entries2 = read_pak(temp)                            # verify: only the item list changed
    assert len(entries2) == len(entries)
    for old, now in zip(entries, entries2):
        assert old['hash'] == now['hash']
        if old is target:
            assert file_data(d2, now, decode_xmem=True) == new
        else:
            assert d[old['offset']:old['offset'] + old['packed']] == d2[now['offset']:now['offset'] + now['packed']]
    os.replace(temp, pak)
    print(f'{ITEMS}: {len(added)} rider gear items added: ' + ', '.join(added))
    return added


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
