"""Set up a DLC pack's sound for PC: event project (.fev) + PC sound banks.

How the PC game finds DLC sound: the DLC pack loader (0x653e90) reads pack<N>/soundproject.xml
(<soundproject file="TrialsEvo_DLC<N>.fev">) and loads that project from soundsystem\\win\\ like
the base game's TrialsFMX.fev. FMOD opens banks through the game's file callback (0x7642d0), which
turns "<bank>.fsb" into "<bank>.pak" and opens it through the content manager, so the banks are
loose files soundsystem\\win\\<bank>.pak next to the pack's data\\ folder (like the base game's
build\\data\\soundsystem\\win\\*.pak, which are plain FSB4 files).

The banks themselves are the Xbox XMA banks converted to PC PCM (same samples, same order, same
FSB header hash) and must already be in <dlc_pc root>\\<pack>\\soundsystem\\win\\<bank>.fsb.

The Xbox .fev (soundsystem/xbox/TrialsEvo_DLC<N>.fev in data.pak, XMem) has the same FEV1 layout
as the PC one. Its bank table (after the project name: u32 count, then per bank u32 flags,
u32 max streams, 8-byte FSB header hash, u32 name length, name) also lists the base game's banks,
with Xbox hashes; FMOD checks a bank's hash against its FSB header, so those entries get the PC
banks' hashes. Hash d41d8cd98f00b204 means "not built" (no file needed, e.g. "Engines"). Xbox
bank flags 0x200 become 0x100, the value the PC project uses for the same banks.

The level songs (Music/Tracks/NN) are set to loop on the way; see loop_songs().

Usage: python tools/dlc_sound.py <pack folder name> [<dlc_pc root>] [<PC build\\data>]
       python tools/dlc_sound.py --loop-installed <game folder>   (existing install, backed up once)
Writes <dlc_pc root>\\<pack>\\soundsystem\\win\\TrialsEvo_DLC<N>.fev and renames the .fsb banks to
.pak. Install by linking DLCContent\\<pack>\\soundsystem to that folder's soundsystem.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from pak import read_pak, file_data                  # noqa: E402
from dlc_events import name_hash                     # noqa: E402

PACKS = {"OriginofPain": 1, "RidersofDoom": 2}
DEFAULT_ROOT = r"O:\trialsevo\dlc_pc"
DEFAULT_PC = r"C:\Program Files (x86)\Steam\steamapps\common\TrialsPC\build\data"
NOT_BUILT = bytes.fromhex("d41d8cd98f00b204")


def bank_table(fev, project):
    """Offset of the bank count and [(entry offset, flags, streams, hash, name)]."""
    if fev[:4] != b"FEV1":
        raise ValueError("not an FMOD FEV1 file")
    name = project.encode() + b"\0"
    o = fev.find(struct.pack("<I", len(name)) + name)        # project name (u32 len, text, NUL)
    if o < 0 or o > 0x400:
        raise ValueError(f"project name {project} not in the FEV header")
    o += 4 + len(name)
    count_at = o
    count = struct.unpack_from("<I", fev, o)[0]
    o += 4
    banks = []
    for _ in range(count):
        flags, streams = struct.unpack_from("<II", fev, o)
        h = fev[o + 8:o + 16]
        n = struct.unpack_from("<I", fev, o + 16)[0]
        name = fev[o + 20:o + 20 + n - 1].decode("ascii")
        banks.append((o, flags, streams, h, name))
        o += 20 + n
    return count_at, banks


def sound_definitions(fev):
    """(offset of the sound definition list, [sound definition name]) from an FEV1 project.

    Each record: u32 name length, name, u32 property index, u32 wave count, then per wave u32 type,
    u32 weight and for type 0 (a wave) the file name, bank name, u32 index in bank, u32 length ms.
    The list is found as the one place a count is followed by that many records that parse."""
    def text(o):
        n = struct.unpack_from("<I", fev, o)[0]
        return fev[o + 4:o + 3 + n].decode("latin1"), o + 4 + n

    def record(o):
        name, o = text(o)
        _, waves = struct.unpack_from("<II", fev, o)
        if not name.startswith("/") or waves > 200:
            raise ValueError
        o += 8
        for _ in range(waves):
            kind = struct.unpack_from("<I", fev, o)[0]
            o += 8
            if kind == 0:
                _, o = text(o)
                _, o = text(o)
                o += 8
            elif kind == 1:
                o += 8
            elif kind not in (2, 3):
                raise ValueError
        return name, o

    for m in re.finditer(rb"(?s)....[\x02-\xff]\x00\x00\x00/", bytes(fev)):    # count, name length, "/"
        count = struct.unpack_from("<I", fev, m.start())[0]
        if not 10 < count < 5000:
            continue
        o, names = m.start() + 4, []
        try:
            for _ in range(count):
                name, o = record(o)
                names.append(name)
        except (ValueError, struct.error, UnicodeDecodeError):
            continue
        return m.start(), names
    raise ValueError("no sound definition list in the FEV")


def loop_songs(fev, num):
    """Make the pack's level songs (events Music/Tracks/NN) loop, like the base game's own music.

    The packs ship them as one-shot sound instances (loop field 1), so a level's song played once
    and then left silence; the DLC songs are only 40-95 s long (base songs run 3-6 min). Each event
    has one instance of /DLC<N>/Music/Track_NN: u16 flags, u16 sound definition, f32 start 0.0,
    f32 length 1.0, u32 0, u32 loop field, i32 loop count -1. Loop field 0 is what the pack's own
    "Music Clip" editor sounds use for the same songs (their _NoDelay definitions), and they loop.
    Returns the number of instances changed; 0 when already looping."""
    start, names = sound_definitions(fev)
    song = re.compile(rf"^/DLC{num}/Music/Track_\d+$")
    songs = {i for i, n in enumerate(names) if song.match(n)}
    if not songs:
        raise ValueError(f"no /DLC{num}/Music/Track_NN sound definitions")
    pattern = re.compile(rb"(?s)..(..)\x00\x00\x00\x00\x00\x00\x80\x3f\x00\x00\x00\x00([\x00\x01])\x00\x00\x00\xff\xff\xff\xff")
    found, changed = set(), 0
    for m in pattern.finditer(bytes(fev[:start])):
        sd = struct.unpack_from("<H", m.group(1))[0]
        if sd not in songs:
            continue
        found.add(sd)
        if m.group(2) == b"\x01":
            fev[m.start(2)] = 0
            changed += 1
    if found != songs:
        raise ValueError(f"{len(songs) - len(found)} of the pack's songs have no event instance")
    return changed


def loop_installed(game):
    """Make the installed packs' level songs loop (projects backed up once). Packs changed."""
    import shutil
    changed = []
    for pack, num in PACKS.items():
        fev_path = os.path.join(game, "datapack", "DLCContent", pack, "soundsystem", "win", f"TrialsEvo_DLC{num}.fev")
        if not os.path.exists(fev_path):
            continue
        with open(fev_path, "rb") as f:
            fev = bytearray(f.read())
        if not loop_songs(fev, num):
            continue
        backup, temp = fev_path + ".before-song-loop", fev_path + ".song-loop.tmp"
        if not os.path.exists(backup):
            shutil.copy2(fev_path, backup)
        with open(temp, "wb") as f:
            f.write(fev)
        os.replace(temp, fev_path)
        changed.append(pack)
    return changed


def main(pack, root=DEFAULT_ROOT, pc=DEFAULT_PC):
    num = PACKS[pack]
    out_dir = os.path.join(root, pack, "soundsystem", "win")
    d, files = read_pak(os.path.join(root, pack, "data", "data.pak"))
    want = name_hash(f"soundsystem/xbox/TrialsEvo_DLC{num}.fev")
    entry = next(f for f in files if f["hash"] == want)
    fev = bytearray(file_data(d, entry, decode_xmem=True))

    pc_dir = os.path.join(pc, "soundsystem", "win")
    pc_banks = {}
    for fn in os.listdir(pc_dir):
        if fn.lower().endswith(".pak"):
            with open(os.path.join(pc_dir, fn), "rb") as f:
                head = f.read(32)
            if head[:4] == b"FSB4":
                pc_banks[fn[:-4].lower()] = head[24:32]

    _, banks = bank_table(bytes(fev), f"TrialsEvo_DLC{num}")
    for o, flags, streams, h, name in banks:
        own = os.path.join(out_dir, name + ".fsb")
        if not os.path.exists(own):
            own = os.path.join(out_dir, name + ".pak")
        if name.lower() in pc_banks:
            source, new_hash = "base game", (h if h == NOT_BUILT else pc_banks[name.lower()])
        elif os.path.exists(own):
            with open(own, "rb") as f:
                file_hash = f.read(32)[24:32]
            source, new_hash = "pack", h
            if h != NOT_BUILT and file_hash != h:
                sys.exit(f"{name}: converted bank hash {file_hash.hex()} != project {h.hex()}")
        elif h == NOT_BUILT:
            source, new_hash = "not built", h
        else:
            sys.exit(f"{name}: no PC bank (expected {own})")
        new_flags = 0x100 if flags == 0x200 else flags
        fev[o:o + 4] = struct.pack("<I", new_flags)
        fev[o + 8:o + 16] = new_hash
        print(f"  {name:22} {source:9} flags {flags:#06x}->{new_flags:#06x} hash {h.hex()}->{new_hash.hex()}")

    print(f"  level songs set to loop: {loop_songs(fev, num)}")
    os.makedirs(out_dir, exist_ok=True)
    fev_path = os.path.join(out_dir, f"TrialsEvo_DLC{num}.fev")
    with open(fev_path, "wb") as f:
        f.write(fev)
    renamed = 0
    for fn in os.listdir(out_dir):
        if fn.lower().endswith(".fsb"):
            src = os.path.join(out_dir, fn)
            dst = src[:-4] + ".pak"
            if os.path.exists(dst):
                os.remove(dst)
            os.rename(src, dst)
            renamed += 1
    print(f"{pack}: wrote {fev_path} ({len(fev)} bytes), renamed {renamed} banks to .pak")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--loop-installed":
        print("level songs set to loop in: " + (", ".join(loop_installed(sys.argv[2])) or "nothing (already looping)"))
    elif len(sys.argv) < 2 or sys.argv[1] not in PACKS:
        sys.exit(__doc__)
    else:
        main(*sys.argv[1:])
