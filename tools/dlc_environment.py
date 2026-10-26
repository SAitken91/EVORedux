"""Add PC-only fogStartDist to Xbox DLC presets, preserving all other pak entries.

Usage: python dlc_environment.py SOURCE_CONFIGS_PAK OUTPUT_PAK
The PC weather reader dereferences fogStartDist unconditionally (0x4f941b).
Xbox presets omit it. Zero is the value in PC's corresponding base presets.
"""
import pathlib
import re
import sys
import xml.etree.ElementTree as ET
import zlib
from pak import read_pak, file_data
from dlc_unpack import names_of
from dlc_convert import write_pak

def convert(source, destination):
    data, entries = read_pak(source)
    replacements = {}
    for name, entry in names_of(data, entries):
        if not re.fullmatch(r'environment_presets[12]\.xml', name):
            continue
        original = file_data(data, entry, decode_xmem=True)
        def add_default(match):
            tag = match.group()
            if b'fogStartDist=' in tag:
                return tag
            return tag[:-2] + b' fogStartDist="0"/>'
        changed = re.sub(rb'<weathercondition\s[^>]+/>', add_default, original)
        root = ET.fromstring(changed)
        assert all('fogStartDist' in e.attrib for e in root.findall('./weatherconditions/weathercondition'))
        assert len(root.findall('./weatherconditions/weathercondition')) > 0
        if original != changed:
            replacements[entry['hash']] = changed
            print(name, 'added PC fog start distance')
    assert replacements, 'No missing PC weather fields found; input left untouched.'
    output = []
    for entry in entries:
        h = entry['hash']
        if h in replacements:
            payload = replacements[h]
            output.append((h,8,zlib.compress(payload,9),len(payload)))
        else:
            output.append((h,entry['method'],data[entry['offset']:entry['offset']+entry['packed']],entry['size']))
    write_pak(destination,output)
    result, check = read_pak(destination)
    assert len(check) == len(entries)
    for old,new in zip(entries,check):
        assert old['hash'] == new['hash']
        if old['hash'] in replacements:
            assert file_data(result,new,decode_xmem=True) == replacements[old['hash']]
        else:
            assert old['method'] == new['method'] and old['size'] == new['size']
            assert data[old['offset']:old['offset']+old['packed']] == result[new['offset']:new['offset']+new['packed']]
    print('Verified:',len(entries),'entries;',len(replacements),'changed')

if __name__ == '__main__':
    assert pathlib.Path(sys.argv[1]).resolve() != pathlib.Path(sys.argv[2]).resolve()
    convert(sys.argv[1],sys.argv[2])
