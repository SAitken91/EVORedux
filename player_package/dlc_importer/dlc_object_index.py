"""Stage per-world PC+DLC object VT indices and noncolliding raw filenames.

Also stages each pack's object decal table (vtex/decalsObjects.dat from its data.pak, already
converted by dlc_vtex.py) as build\data\vtex\decalsObjects_dlc<world>.dat: the game hardcodes the
base table for every world, and the client shim switches DLC worlds to these (patchy seabed on
Buccaneer Bay and other Paine Island tracks without them).

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


def rows(data):
    count, = struct.unpack_from('<I', data)
    assert len(data) == 4 + count * 11
    result = list(struct.iter_unpack('<IIHB', data[4:]))
    assert len({r[0] for r in result}) == count
    assert result == sorted(result)
    return result


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
    for world, pack in enumerate(('OriginofPain', 'RidersofDoom'), 1):
        root = args.game / 'datapack/DLCContent' / pack
        if not (root / 'data/data.pak').exists():
            print(f'{pack}: not installed, skipped', flush=True)
            continue
        hdr_name = f'vtex/vtex_objects{world+1}.raw.hdr'
        data, entries = read_pak(str(root / 'data/data.pak'))
        hdr = file_data(data, next(e for e in entries if e['hash'] == name_hash(hdr_name)))
        dlc = rows(hdr)
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
            (args.output / decals_name).write_bytes(file_data(data, decals))
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
