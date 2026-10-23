"""Import historical Track Central leaderboards from the preserved track archive.

Source: O:\\trialsevo\\library (catalog.sqlite3 + sha256 blobs). Each archived track bundle has a
leaderboard_g.lbd: the game's own cached copy of that track's global board from when the
official servers were up.

lbd layout (decoded from the files):
  header 21 bytes : u32 magic 0x75dabeb0, u8 9, leaderboard key..., u8 entry count at offset 20
  entry  41 bytes : u64 pid, 3 x 00, u8 rank, u64 score, u64 descriptor, 13 trailing bytes
Some entries carry an inline replay after them, which shifts later entries; the importer resyncs
on the next entry's signature (zero padding, expected rank, score top byte 0x3F/0x40, same
descriptor as the board) and verifies ranks are 1..n and scores descend.

The bundle id's suffix is the game's online track id (e.g. ...-0-0000000000dac = 3500).
Rows are stored in ugc_runs with historical=1; they are never mixed up with live runs.

Usage:  python tools/import_history.py [archive_library_dir]
"""
import os
import sqlite3
import struct
import sys
import time

LIBRARY = sys.argv[1] if len(sys.argv) > 1 else r"O:\trialsevo\library"
DB = os.path.join(os.path.dirname(__file__), "..", "trials.db")
MAGIC = 0x75DABEB0
ENTRY = 41


def s64(v):
    return struct.unpack("<q", struct.pack("<Q", v & 0xFFFFFFFFFFFFFFFF))[0]


def looks_like_entry(d, o, rank, desc):
    if o + ENTRY > len(d) or d[o + 8:o + 11] != b"\0\0\0" or d[o + 11] != rank:
        return False
    score, d2 = struct.unpack_from("<QQ", d, o + 12)
    return (score >> 56) in (0x3F, 0x40) and (desc is None or d2 == desc)


def parse_lbd(d):
    """Return [(pid, rank, score, descriptor)] or raise ValueError."""
    if len(d) < 21 or struct.unpack_from("<I", d, 0)[0] != MAGIC:
        raise ValueError("bad magic")
    count, out, cursor, desc = d[20], [], 21, None
    for rank in range(1, count + 1):
        o = cursor
        while o + ENTRY <= len(d) and not looks_like_entry(d, o, rank, desc):
            o += 1
        if o + ENTRY > len(d):
            break                                   # remaining entries not recoverable
        pid = struct.unpack_from("<Q", d, o)[0]
        score, desc_here = struct.unpack_from("<QQ", d, o + 12)
        desc = desc if desc is not None else desc_here
        out.append((pid, rank, score, desc_here))
        cursor = o + ENTRY
    for (_, _, a, _), (_, _, b, _) in zip(out, out[1:]):
        if b > a:
            raise ValueError("scores not descending")
    return out


def main():
    cat = sqlite3.connect(os.path.join(LIBRARY, "catalog.sqlite3"))
    db = sqlite3.connect(DB)
    db.execute("CREATE TABLE IF NOT EXISTS ugc_runs (id INTEGER PRIMARY KEY, pid INTEGER, "
               "track_id INTEGER, score INTEGER, descriptor INTEGER, received REAL)")
    cols = [r[1] for r in db.execute("PRAGMA table_info(ugc_runs)")]
    if "historical" not in cols:
        db.execute("ALTER TABLE ugc_runs ADD COLUMN historical INTEGER NOT NULL DEFAULT 0")
    db.execute("DELETE FROM ugc_runs WHERE historical=1")          # re-import is idempotent

    boards = full = partial = failed = entries = 0
    players = set()
    rows = cat.execute("SELECT a.track_id, a.sha256 FROM assets a WHERE a.filename='leaderboard_g.lbd'").fetchall()
    for bundle, sha in rows:
        track_id = int(bundle.rsplit("-", 1)[1], 16)
        data = open(os.path.join(LIBRARY, "blobs", sha[:2], sha), "rb").read()
        try:
            parsed = parse_lbd(data)
        except ValueError:
            failed += 1
            continue
        boards += 1
        full += len(parsed) == data[20]
        partial += 0 < len(parsed) < data[20]
        for pid, rank, score, desc in parsed:
            db.execute("INSERT INTO ugc_runs (pid, track_id, score, descriptor, received, historical) "
                       "VALUES (?, ?, ?, ?, ?, 1)", (s64(pid), track_id, s64(score), s64(desc), 0))
            players.add(pid)
        entries += len(parsed)
    db.commit()
    print(f"{len(rows)} archived boards: {boards} imported ({full} complete, {partial} partial), "
          f"{failed} unreadable; {entries} historical entries from {len(players)} players")


if __name__ == "__main__":
    main()
