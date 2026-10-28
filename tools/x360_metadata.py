"""Rewrite Xbox 360 track metadata (metadata.mda) in the PC format.

The PC writer (TrackLoader::saveMetadata 0x4e8f00) stores u32 fields big-endian but u64 fields
little-endian; the 360 (version 8) stores everything big-endian. Read on PC, every u64 of a 360
file comes out byte-swapped: the track's online id (e.g. -6454783066417135616), creator, upload
date and score formats. The Play Tracks grid keys its tiles on that id, and with 360 tracks
installed it went blank whenever the menu was rebuilt (after viewing or playing a track).

Field order (same on both): magic, version, u64 creator, 4 x u32, u32 rate, u64 date, u8 bikes,
u64 original id, u64, u8 DLC bike, u64 content id, u8 difficulty, u8 game type, 4 x u32 (time,
downloads, votes, stars), u8, u8, u64 score formats, 5 x (u32, u32) medals, u8 n + description,
u8 flag (+ one extra trailing byte on the 360).
The creator becomes the low 32 bits of the XUID: the pid the server's track library answers for.

Usage: python tools/x360_metadata.py <folder of track folders> [more folders...]
"""
import os
import struct
import sys

MAGIC = bytes.fromhex("da7ababe")
FIELDS = ("Q", "I", "I", "I", "I", "I", "Q", "B", "Q", "Q", "B", "Q", "B", "B", "I", "I", "I", "I", "B", "B", "Q")
RATE, ORIGINAL_ID, AFTER_ORIGINAL, CONTENT_ID = 5, 8, 9, 11      # indices in FIELDS


def read(b, u64, u32):
    """(values, medals, description, flag) of a version 7/8 metadata file."""
    o, values = 5, []
    for f in FIELDS:
        values.append(struct.unpack_from((u64 if f == "Q" else u32) + f, b, o)[0])
        o += struct.calcsize(f)
    medals = [struct.unpack_from(u32 + "II", b, o + 8 * i) for i in range(5)]
    o += 40
    n = b[o]
    desc = b[o + 1:o + 1 + n]
    o += 1 + n
    return values, medals, desc, (b[o] if o < len(b) else 0)


def write_pc(values, medals, desc, flag):
    out = bytearray(MAGIC + b"\x07")
    for f, v in zip(FIELDS, values):
        out += struct.pack(("<" if f == "Q" else ">") + f, v)
    for a, b in medals:
        out += struct.pack(">II", a, b)
    return bytes(out + bytes([len(desc)]) + desc + bytes([flag]))


def convert(b, xuid=None, played=None):
    """PC metadata for a 360 file. `played`: the PC-format file the game already rewrote from
    garbage values; its rating (the only field the player sets) is kept."""
    if b[:4] != MAGIC or b[4] != 8:
        return None
    values, medals, desc, flag = read(b, ">", ">")
    if xuid is not None:
        values[0] = xuid & 0xFFFFFFFF
    # As a Track Central download writes it (SharingManager::saveDownloadedTrack): original id =
    # content id, and the u64 after it zeroed.
    values[ORIGINAL_ID], values[AFTER_ORIGINAL] = values[CONTENT_ID], 0
    if played is not None:
        values[RATE] = read(played, "<", ">")[0][RATE]
    return write_pc(values, medals, desc, flag)


def convert_folders(roots, originals=None, log=print):
    """Rewrite 360-format metadata.mda in every track folder. `originals`: folder holding the
    converter's untouched copies, used for folders whose file the game already rewrote."""
    done = 0
    for root in roots:
        for name in os.listdir(root):
            path = os.path.join(root, name, "metadata.mda")
            if not os.path.isfile(path) or not name.startswith("0009"):
                continue
            cur = open(path, "rb").read()
            src, played = cur, None
            if cur[4:5] == b"\x07" and originals:
                orig = os.path.join(originals, name, "metadata.mda")
                if not os.path.isfile(orig):
                    continue
                src, played = open(orig, "rb").read(), cur
            new = convert(src, int(name[:16], 16), played)
            if new and new != cur:
                with open(path + ".tmp", "wb") as f:
                    f.write(new)
                os.replace(path + ".tmp", path)
                done += 1
    log(f"metadata: {done} rewritten in PC format")
    return done


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    convert_folders(sys.argv[1:])
