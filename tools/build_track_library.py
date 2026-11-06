"""Build track_library.db (Track Central library) from converted Xbox 360 tracks.

Input: the output folder of tools/x360_tracks.py (one PC track folder per track plus
conversion_report.csv). Tracks that need DLC objects are left out until the engine loads them
(see X360_TRACKS_HANDOFF.md); --all includes them anyway.

Each track keeps its own Track Central id (the one in its folder name and metadata), so players
who installed the same tracks from the pack already have them.

--pc-archive reads PC Track Central tracks instead (folders as the game saves them, e.g. the
community's TrialsEvo-PCTC archive; no report file). PC tracks only record their creator's original
account number, so they are listed under the legacy range (LEGACY_PID_BASE + number, shown as
Legacy#<number> like the historical scores): resolving original PC pids made the client hide installed
Track Central tracks (see quazal.legacy_board_pid). --skip-db leaves out ids another library already
has (two PC ids are also Xbox 360 ids).

Usage: python tools/build_track_library.py <converted tracks folder> [--db track_library.db] [--all]
       python tools/build_track_library.py <PC tracks folder> --pc-archive [--skip-db live.db]
"""
import argparse
import csv
import os
import sqlite3
from collections import Counter, defaultdict
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from track_library import TrackLibrary, flags_for, track_dlc  # noqa: E402

FILETIME_UNIX_EPOCH = 116444736000000000
LEGACY_PID_BASE = 2_000_000_000          # quazal.LEGACY_PID_BASE
# Tracks that convert but don't play right on PC (content id: reason).
EXCLUDED = {
    0x21a6b: "Tony Hawk's PS HD (redlynxlive): TRIg trigger chunk, map breaks on PC",
}


def quazal_datetime(t):
    g = time.gmtime(t)
    return (g.tm_sec | g.tm_min << 6 | g.tm_hour << 12 | g.tm_mday << 17
            | g.tm_mon << 22 | g.tm_year << 26)


def parse_metadata(b):
    """metadata.mda as the Xbox 360 writes it (version 8, big-endian) or as x360_metadata.py
    rewrites it (PC version 7: u64 little-endian, u32 big-endian). Field order follows the PC
    writer TrackLoader::saveMetadata (0x4e8f00)."""
    if b[:4] != bytes.fromhex("da7ababe") or b[4] not in (7, 8):
        raise ValueError("not a version 7/8 metadata file")
    u64 = ">" if b[4] == 8 else "<"
    o = 5

    def rd(fmt):
        nonlocal o
        v = struct.unpack_from((u64 if "Q" in fmt else ">") + fmt, b, o)
        o += struct.calcsize(fmt)
        return v[0] if len(v) == 1 else v
    m = {"creator": rd("Q"), "ints": [rd("i") for _ in range(4)], "rate": rd("I"), "date": rd("Q"),
         "bikes": rd("B"), "original_id": rd("Q"), "q48": rd("Q"), "dlc_bike": rd("B"),
         "content_id": rd("Q"), "difficulty": rd("B"), "game_type": rd("B"), "u50": rd("I"),
         "downloads": rd("I"), "votes": rd("I"), "stars": rd("I"), "b60": rd("B"), "b61": rd("B"),
         "score_formats": rd("Q"), "medals": [rd("II") for _ in range(5)]}
    n = rd("B")
    m["description"] = b[o:o + n]
    o += n
    m["flag31"] = b[o] if o < len(b) else 0
    return m


def metadata_blob(m):
    """The Track Central metadata blob (SharingManager::uploadTrackContinue 0x719cf0):
    u8 allowed bikes, u8 version 4, u32 description length, description, u64 score formats."""
    desc = m["description"]
    return (bytes([m["bikes"], 4]) + struct.pack("<I", len(desc)) + desc
            + struct.pack("<Q", m["score_formats"]))


def screenshot(thn):
    """Track Central's screenshot blob is a PC thumbnail.thn without its first 9 bytes:
    u32 size (incl. these 12 header bytes), u32 width, u32 height, JPEG. The game reads the
    first u32 as the size (a bare JPEG showed as "Too big screenshot (0xE0FFD8FF)").
    360 thumbnails must be converted first (x360_thumbnails.py); unconverted ones give none."""
    if thn[:5] != bytes.fromhex("ce7a1e7501") or thn[21:24] != b"\xff\xd8\xff":
        return b""
    return thn[9:]


def creator_pid(package):
    """Low 32 bits of the creator XUID in the folder name: the pid the game looks up."""
    return int(package[:16], 16) & 0xFFFFFFFF


def pc_archive_rows(source):
    """Report-style rows for PC Track Central folders (creator = original PC account number)."""
    rows = []
    for package in sorted(os.listdir(source)):
        folder = os.path.join(source, package)
        if not os.path.isfile(os.path.join(folder, "metadata.mda")):
            continue
        try:
            name = open(os.path.join(folder, "displayname"), "rb").read().decode("utf-8", "replace").strip(chr(0))
        except OSError:
            name = ""
        pid = LEGACY_PID_BASE + creator_pid(package)
        rows.append({"status": "ok", "creator": f"Legacy#{pid - LEGACY_PID_BASE}", "track": name,
                     "package": package, "dlc_objects": "0", "owner": pid})
    return rows


def dlc_sample(source, rows, count):
    """The count best-rated converted tracks that need DLC (DLC objects or a DLC world)."""
    picks = []
    for r in rows:
        if r["status"] != "ok":
            continue
        folder = os.path.join(source, r["package"])
        try:
            track = open(os.path.join(folder, "track.trk"), "rb").read()
            if not track_dlc(track):
                continue
            m = parse_metadata(open(os.path.join(folder, "metadata.mda"), "rb").read())
        except (OSError, ValueError, struct.error):
            continue
        if m["content_id"] in EXCLUDED or not 1 <= m["stars"] <= 5:
            continue
        picks.append(((-m["stars"], m["difficulty"] != 4, -min(m["votes"], 10_000_000)), r))
    picks.sort(key=lambda p: p[0])
    return [r for _, r in picks[:count]]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source")
    ap.add_argument("--db", default="track_library.db")
    ap.add_argument("--all", action="store_true", help="include tracks that use DLC objects")
    ap.add_argument("--dlc-sample", type=int, metavar="N",
                    help="add only the N best-rated tracks that need DLC (extreme ones first among equal "
                         "ratings), e.g. to try DLC tracks on Track Central a batch at a time")
    ap.add_argument("--pc-archive", action="store_true", help="source holds PC Track Central track folders")
    ap.add_argument("--skip-db", metavar="DB", help="leave out content ids this library database already has")
    args = ap.parse_args()

    lib = TrackLibrary(args.db)
    added = skipped = 0
    skip = set()
    if args.skip_db:
        skip = {r[0] for r in sqlite3.connect(args.skip_db).execute("SELECT content_id FROM tracks")}
    if args.pc_archive:
        rows = pc_archive_rows(args.source)       # legacy-range creators: quazal names them itself
    else:
        rows = list(csv.DictReader(open(os.path.join(args.source, "conversion_report.csv"), encoding="utf-8")))
        # One name per creator pid: the archive's most common creator folder for that XUID.
        names = defaultdict(Counter)
        for r in rows:
            if r["status"] == "ok":
                names[creator_pid(r["package"])][r["creator"] or "Unknown"] += 1
        for pid, counter in names.items():
            lib.set_creator(pid, counter.most_common(1)[0][0])
    if args.dlc_sample:
        rows = dlc_sample(args.source, rows, args.dlc_sample)
    for r in rows:
        if r["status"] != "ok" or (r["dlc_objects"] not in ("0", "") and not (args.all or args.dlc_sample)):
            continue
        folder = os.path.join(args.source, r["package"])
        try:
            m = parse_metadata(open(os.path.join(folder, "metadata.mda"), "rb").read())
            track = open(os.path.join(folder, "track.trk"), "rb").read()
            shot = screenshot(open(os.path.join(folder, "thumbnail.thn"), "rb").read())
        except (OSError, ValueError, struct.error) as e:
            print(f"skip {r['package']}: {e}")
            skipped += 1
            continue
        if m["content_id"] in EXCLUDED:
            print(f"skip {r['track']}: {EXCLUDED[m['content_id']]}")
            skipped += 1
            continue
        if m["content_id"] in skip:
            print(f"skip {r['track']}: id {m['content_id']} already in {args.skip_db}")
            skipped += 1
            continue
        uploaded = (m["date"] - FILETIME_UNIX_EPOCH) / 1e7
        if not 1262304000 < uploaded < 1893456000:      # outside 2010..2030: unknown
            uploaded = 1356998400
        stars = m["stars"] if 1 <= m["stars"] <= 5 else 0
        lib.put(content_id=m["content_id"], owner_pid=r.get("owner") or creator_pid(r["package"]),
                creator=r["creator"], name=r["track"], folder=r["package"],
                flags=flags_for(m["game_type"], m["difficulty"], m["flag31"]),
                p11=m["ints"][0], p12=m["ints"][1], p13=m["ints"][2], p14=m["ints"][3],
                uploaded=quazal_datetime(uploaded),
                downloads=m["downloads"] if m["downloads"] < 100_000_000 else 0,
                rating=stars * 20, votes=m["votes"] if m["votes"] < 10_000_000 else 0,
                source="pc" if args.pc_archive else "xbox360", metablob=metadata_blob(m), track=track,
                shot=shot)
        added += 1
    lib.db.commit()
    lib.reload()
    print(f"{added} tracks added ({skipped} skipped); library now has {len(lib)} tracks, "
          f"{len(lib.creators)} creators")


if __name__ == "__main__":
    main()
