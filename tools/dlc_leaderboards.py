"""Give the converted DLC tracks leaderboard ids the PC game can use (run after dlc_convert.py).

Two problems with the Xbox 360 tracks.xml leaderboard_id values on PC:
  - Tracks on the world map with leaderboard_id="0" (Paine Island's supercross events, e.g. track 203)
    crash the event menu: TrackInfoManager only registers non-zero ids, so the menu's
    getGameModeSupportForLB(0) lookup returns null and is dereferenced at 0x6f44a7.
  - Pack 1 ids 120-130 and every pack 2 id (131-165) are already used by PC tracks, so those
    events would share (and corrupt) PC leaderboards.
Each such track gets an unused id from 178 up (ids are a byte). DLC ids that are already free
(89-119) are kept. Only pack<N>/tracks.xml in configs.pak changes; it is stored zlib-compressed
(method 8) like the PC game's own tracks.xml. Running it again changes nothing.

Usage: python tools/dlc_leaderboards.py [<dlc_pc root>] [<PC data.pak>]
Writes <dlc_pc root>/leaderboard_ids.json (track id -> new leaderboard id) for the server.
"""
import json
import os
import re
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data                  # noqa: E402
from dlc_unpack import names_of                       # noqa: E402
from dlc_convert import write_pak                     # noqa: E402
from gen_boards import DEFAULT_PAK, read_pak_file      # noqa: E402
from xmem import xmem_decompress, is_xmem             # noqa: E402

PACKS = (("OriginofPain", 1), ("RidersofDoom", 2))
FIRST_NEW_ID = 178
SPECIAL_TRACKS = 65000          # 65000+ are per-pack placeholder tracks, never shown as events
TRACK_RE = re.compile(rb'<Track id="(\d+)"[^>]*?leaderboard_id="(\d+)"')


def track_ids(xml):
    return [(int(t), int(lb), m) for m in TRACK_RE.finditer(xml) for t, lb in [m.groups()]]


def load_tracks(pak_path, pack):
    d, files = read_pak(pak_path)
    for name, f in names_of(d, files):
        if name == f"pack{pack}/tracks.xml":
            b = file_data(d, f)
            return d, files, f, (xmem_decompress(b) if f["method"] == 4 and is_xmem(b) else b)
    raise KeyError(f"pack{pack}/tracks.xml not in {pak_path}")


def main(root, pc_pak):
    used = {lb for _t, lb, _m in track_ids(read_pak_file(pc_pak, "pack0/tracks.xml"))}
    pc_ids = set(used)
    packs = []
    for folder, pack in PACKS:
        path = os.path.join(root, folder, "data", "configs.pak")
        d, files, entry, xml = load_tracks(path, pack)
        packs.append((path, pack, d, files, entry, xml))
        used |= {lb for t, lb, _m in track_ids(xml) if lb and t < SPECIAL_TRACKS and lb not in pc_ids}

    next_id, mapping = FIRST_NEW_ID, {}
    for path, pack, d, files, entry, xml in packs:
        changes = []
        for t, lb, m in track_ids(xml):
            if t >= SPECIAL_TRACKS or (lb and lb not in pc_ids):
                continue
            while next_id in used:
                next_id += 1
            if next_id > 255:
                sys.exit("out of leaderboard ids")
            used.add(next_id)
            changes.append((m.start(2), m.end(2), next_id))
            mapping[t] = {"pack": pack, "old": lb, "new": next_id}
        if not changes:
            print(f"pack{pack}: nothing to change")
            continue
        for start, end, new in reversed(changes):
            xml = xml[:start] + str(new).encode() + xml[end:]
        out = []
        for f in files:
            raw = d[f["offset"]:f["offset"] + f["packed"]]
            if f is entry:
                out.append((f["hash"], 8, zlib.compress(xml, 9), len(xml)))
            else:
                out.append((f["hash"], f["method"], raw, f["size"]))
        write_pak(path, out)
        print(f"pack{pack}: {len(changes)} tracks renumbered in {path}")

    if mapping:
        with open(os.path.join(root, "leaderboard_ids.json"), "w") as w:
            json.dump(mapping, w, indent=1)
    for t, m in sorted(mapping.items()):
        print(f"  track {t}: leaderboard {m['old']} -> {m['new']}")


def apply_fixed(root, mapping):
    """Give each pack's tracks the leaderboard ids from a saved mapping (the ids the live server's
    boards use), so a pack converted on its own gets the same ids as when both packs are converted
    together. Same edit as main(): only the leaderboard_id digits change."""
    for folder, pack in PACKS:
        path = os.path.join(root, folder, "data", "configs.pak")
        if not os.path.exists(path):
            continue
        d, files, entry, xml = load_tracks(path, pack)
        changes = [(m.start(2), m.end(2), mapping[str(t)]["new"]) for t, lb, m in track_ids(xml)
                   if str(t) in mapping and mapping[str(t)]["pack"] == pack and lb != mapping[str(t)]["new"]]
        if not changes:
            print(f"pack{pack}: nothing to change")
            continue
        for start, end, new in reversed(changes):
            xml = xml[:start] + str(new).encode() + xml[end:]
        out = []
        for f in files:
            raw = d[f["offset"]:f["offset"] + f["packed"]]
            if f is entry:
                out.append((f["hash"], 8, zlib.compress(xml, 9), len(xml)))
            else:
                out.append((f["hash"], f["method"], raw, f["size"]))
        write_pak(path, out)
        print(f"pack{pack}: {len(changes)} tracks renumbered in {path}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else r"O:\trialsevo\dlc_pc",
         sys.argv[2] if len(sys.argv) > 2 else DEFAULT_PAK)
