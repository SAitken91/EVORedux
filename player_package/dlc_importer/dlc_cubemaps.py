"""Recompress the DLC reflection cube maps that dlc_convert.py left in Xbox XMem.

dlc_convert.py looked for the texture payload flag only in the first 96 bytes, but a cube map's
header carries a 6-face table and its flag sits at byte 120, so every pack<N>/images/materials/
<id>_reflection_cube.tex (9 in Origin of Pain, 5 in Riders of Doom) kept its XMem payload. The PC
inflates that payload with zlib, so every shiny DLC surface sampled garbage (the "rainbow" Jet Lag
plane, GitHub issue #2). The decoded pixels are byte-identical to the PC's copies of the same cube
maps, so only the compression changes. dlc_convert.py is fixed for new imports; this repairs a
pack that was already imported.
It also repairs the 256x256 terrain height atlases (8 in Origin of Pain, 7 in Riders of Doom)
whose XMem payload came out 28 bytes larger than the pixels, which the converter's size check
rejected. Terrain decals are composed on the CPU from these, so the DLC seabed and ground decals
drew garbage (white tiles, sign lettering on Buccaneer Bay's seabed).
Usage: python dlc_cubemaps.py SOURCE_DATA_TEX_PAK OUTPUT_PAK
"""
import pathlib
import sys
import zlib
from pak import read_pak
from dlc_unpack import names_of
from dlc_convert import write_pak, unpack_entry, convert_texture, find_xmem_payload, TEXTURE_MAGICS
from xmem import xmem_decompress


def convert(source, destination):
    data, entries = read_pak(source)
    replacements = {}
    for name, entry in names_of(data, entries):
        if entry['method'] not in (0, 4, 8):
            continue
        texture = unpack_entry(data, entry)
        if texture[:3] not in TEXTURE_MAGICS or find_xmem_payload(texture) is None:
            continue
        flag = find_xmem_payload(texture)
        pixels = xmem_decompress(texture[flag + 1:])
        changed, did = convert_texture(texture)
        assert did and changed[:flag + 1] == texture[:flag + 1]
        assert zlib.decompress(changed[flag + 1:]) == pixels
        replacements[entry['hash']] = changed
        print(name, 'payload at', flag + 1, len(pixels), 'pixel bytes, recompressed and round-trip verified')
    assert replacements, 'No XMem textures left; no output written'
    output = []
    for entry in entries:
        h = entry['hash']
        if h in replacements:
            payload = replacements[h]
            output.append((h, 8, zlib.compress(payload, 6), len(payload)))
        else:
            output.append((h, entry['method'], data[entry['offset']:entry['offset'] + entry['packed']], entry['size']))
    write_pak(destination, output)
    result, check = read_pak(destination)
    assert len(check) == len(entries)
    for old, new in zip(entries, check):
        assert old['hash'] == new['hash']
        payload = result[new['offset']:new['offset'] + new['packed']]
        if old['hash'] in replacements:
            assert new['method'] == 8 and zlib.decompress(payload) == replacements[old['hash']]
        else:
            assert old['method'] == new['method'] and old['size'] == new['size']
            assert payload == data[old['offset']:old['offset'] + old['packed']]
    print(len(replacements), 'textures converted; verified entire output archive, unrelated entries unchanged')


if __name__ == '__main__':
    assert pathlib.Path(sys.argv[1]).resolve() != pathlib.Path(sys.argv[2]).resolve()
    convert(sys.argv[1], sys.argv[2])
