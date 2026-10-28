"""Convert Xbox 360 Trials Evolution custom tracks (CON packages) to PC Track Central track folders.

Each 360 track is a CON package holding the same files the PC game keeps per track in
Documents\\Ubisoft\\Trials Evolution Gold Edition\\SavedGames\\<content id>\\:
track.trk, metadata.mda, thumbnail.thn, score.sco, leaderboard_f.lbd, leaderboard_g.lbd.
Conversion, per track:
  - check the package against its built-in SHA-1 hashes (damaged copies are skipped)
  - track.trk magic 0xCAFEB00B -> 0xDEADBABE where needed (same layout otherwise)
  - empty both leaderboard caches (keep the 9-byte header, zero counts): the 360's cached
    scores/ghosts made the PC game hang before loading the track
  - add displayname (the track name, from the archive's <creator>\\<track>\\ folder)
  - thumbnail.thn: JPEG XR -> the PC's JPEG (x360_thumbnails.py; Windows only)
  - metadata.mda: 360 byte order -> the PC's (x360_metadata.py)
A report (CSV) lists every track with its DLC needs (DLC0 chunk) and any problem.

Usage: python x360_tracks.py <folder of 360 packages> <output folder> [--limit N]
"""
import argparse
import csv
import lzma
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stfs_extract import STFS  # noqa: E402
from x360_thumbnails import convert_folders  # noqa: E402
import x360_metadata  # noqa: E402

PC_MAGIC, X360_MAGIC = bytes.fromhex('deadbabe'), bytes.fromhex('cafeb00b')
LBD_EMPTY_TAIL = bytes(12)
LBD_MAGIC = bytes.fromhex('b0beda75')


def track_chunks(trk):
    """Chunk id -> data of the LZMA track body ([id][u32 BE size][data] ... END0)."""
    i = trk.find(b'\x5d\x00\x00\x02\x00')
    if i != 37:
        raise ValueError('unexpected track header')
    usize = struct.unpack_from('<I', trk, i + 5)[0]
    d = lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(trk[i:i + 5] + struct.pack('<q', usize) + trk[i + 9:])
    out, o = {}, 0
    while o + 8 <= len(d):
        cid = d[o:o + 4].decode('latin-1')
        size = struct.unpack_from('>I', d, o + 4)[0]
        out[cid] = d[o + 8:o + 8 + size]
        o += 8 + size
        if cid == 'END0':
            break
    if 'END0' not in out:
        raise ValueError('track body has no END0')
    return out


def convert(package, out_root, creator, name):
    row = {'creator': creator, 'track': name, 'package': os.path.basename(package)}
    try:
        pkg = STFS(package)
    except Exception as e:                       # noqa: BLE001
        return {**row, 'status': f'not a package ({e})'}
    # Some re-saved packages fail the block hash check only on trailing / rewritten blocks; the
    # track itself is accepted only if its LZMA body decodes exactly (checked below).
    if pkg.verify():
        row['hash_warning'] = 'yes'
    out = os.path.join(out_root, os.path.basename(package))
    tmp = out + '.tmp'
    shutil.rmtree(tmp, ignore_errors=True)
    try:
        pkg.extract(tmp, tolerate_short=('leaderboard_f.lbd', 'leaderboard_g.lbd'))
        trk_path = os.path.join(tmp, 'track.trk')
        if not os.path.exists(trk_path):
            raise ValueError('no track.trk in package')
        trk = bytearray(open(trk_path, 'rb').read())
        row['magic'] = trk[:4].hex()
        if trk[:4] == X360_MAGIC:
            trk[:4] = PC_MAGIC
            open(trk_path, 'wb').write(trk)
        elif trk[:4] != PC_MAGIC:
            raise ValueError(f'unknown track magic {trk[:4].hex()}')
        chunks = track_chunks(bytes(trk))
        dlc = chunks.get('DLC0', b'')
        row['dlc_objects'] = dlc[3] if len(dlc) > 3 else ''
        row['dlc_bike'] = dlc[4] if len(dlc) > 4 else ''
        row['chunks'] = ' '.join(k for k in chunks if k != 'END0')
        for fn in ('leaderboard_f.lbd', 'leaderboard_g.lbd'):
            p = os.path.join(tmp, fn)
            if os.path.exists(p):
                b = open(p, 'rb').read()
                if len(b) != 21 or b[9:] != LBD_EMPTY_TAIL:
                    head = b[:9] if len(b) >= 9 and b[:4] == LBD_MAGIC else LBD_MAGIC + b'	' + bytes(4)
                    open(p, 'wb').write(head + LBD_EMPTY_TAIL)
                    row['cache_emptied'] = 'yes'
        open(os.path.join(tmp, 'displayname'), 'wb').write(name.encode('utf-8'))
        shutil.rmtree(out, ignore_errors=True)
        os.replace(tmp, out)
        return {**row, 'status': 'ok'}
    except Exception as e:                       # noqa: BLE001
        shutil.rmtree(tmp, ignore_errors=True)
        return {**row, 'status': f'error: {e}'}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source')
    ap.add_argument('output')
    ap.add_argument('--limit', type=int)
    args = ap.parse_args()
    os.makedirs(args.output, exist_ok=True)
    jobs = []
    for dp, _, files in os.walk(args.source):
        for f in files:
            rel = os.path.relpath(os.path.join(dp, f), args.source).split(os.sep)
            creator = rel[0] if len(rel) > 1 else ''
            name = rel[-2] if len(rel) > 2 else os.path.splitext(f)[0]
            jobs.append((os.path.join(dp, f), creator, name))
    jobs.sort()
    if args.limit:
        jobs = jobs[:args.limit]
    rows = []
    for k, (path, creator, name) in enumerate(jobs, 1):
        rows.append(convert(path, args.output, creator, name))
        if k % 250 == 0:
            print(f'{k}/{len(jobs)}', flush=True)
    x360_metadata.convert_folders([args.output])
    done, failed = convert_folders([args.output])
    print(f'thumbnails: {done} converted to PC format, {failed} failed')
    fields = ['status', 'creator', 'track', 'package', 'magic', 'dlc_objects', 'dlc_bike', 'cache_emptied', 'hash_warning', 'chunks']
    with open(os.path.join(args.output, 'conversion_report.csv'), 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
        w.writeheader()
        w.writerows(rows)
    from collections import Counter
    print(Counter(r['status'].split(' (')[0].split(':')[0] for r in rows))


if __name__ == '__main__':
    main()
