"""Recompress terrain/*.game streaming pages from Xbox XMem to PC zlib.

The first u32 is both the offset-table byte length and the first page offset.
All table entries are absolute starts; EOF ends the final page. The PC inflates
these independently with zlib. Leaving XMem pages causes garbage tile counts
and a stack overwrite in the terrain tile decoder at 0x7ff7d3.
Usage: python dlc_terrain.py SOURCE_DATA_PAK OUTPUT_PAK
"""
import pathlib
import struct
import sys
import zlib
from pak import read_pak, file_data
from dlc_unpack import names_of
from dlc_convert import write_pak
from xmem import xmem_decompress, is_xmem

def convert_pages(data):
    table_size = struct.unpack_from('<I',data)[0]
    assert 4 <= table_size < len(data) and table_size % 4 == 0
    starts = list(struct.unpack_from('<' + str(table_size//4) + 'I', data))
    assert starts[0] == table_size and starts == sorted(set(starts))
    assert starts[-1] < len(data)
    pages, new_starts, offset, converted = [], [], table_size, 0
    for i,(start,end) in enumerate(zip(starts,starts[1:] + [len(data)])):
        original = data[start:end]
        if is_xmem(original):
            decoded = xmem_decompress(original)
            assert len(decoded) == struct.unpack_from('<I',original)[0]
            packed = zlib.compress(decoded,6)
            assert zlib.decompress(packed) == decoded
            converted += 1
        else:
            zlib.decompress(original) # fail closed on an unknown format
            packed = original
        new_starts.append(offset)
        pages.append(packed)
        offset += len(packed)
        if i % 200 == 0: print('page',i,'of',len(starts),flush=True)
    result = struct.pack('<' + str(len(starts)) + 'I',*new_starts) + b''.join(pages)
    assert len(result) == offset
    return result,converted

def convert(source,destination):
    data,entries = read_pak(source)
    replacements = {}
    for name,entry in names_of(data,entries):
        if name.startswith('terrain/') and name.endswith('.game'):
            changed,count = convert_pages(file_data(data,entry,decode_xmem=True))
            assert count, 'Already converted; no output written'
            replacements[entry['hash']] = changed
            print(name,count,'pages converted and individually round-trip verified',flush=True)
    assert replacements
    output = []
    for entry in entries:
        h = entry['hash']
        if h in replacements:
            payload = replacements[h]
            output.append((h,0,payload,len(payload)))
        else:
            output.append((h,entry['method'],data[entry['offset']:entry['offset']+entry['packed']],entry['size']))
    write_pak(destination,output)
    result,check = read_pak(destination)
    assert len(check) == len(entries)
    for old,new in zip(entries,check):
        assert old['hash'] == new['hash']
        payload = result[new['offset']:new['offset']+new['packed']]
        if old['hash'] in replacements:
            assert new['method'] == 0 and payload == replacements[old['hash']]
        else:
            assert old['method'] == new['method'] and old['size'] == new['size']
            assert payload == data[old['offset']:old['offset']+old['packed']]
    print('Verified entire output archive; unrelated entries unchanged',flush=True)

if __name__ == '__main__':
    assert pathlib.Path(sys.argv[1]).resolve() != pathlib.Path(sys.argv[2]).resolve()
    convert(sys.argv[1],sys.argv[2])
