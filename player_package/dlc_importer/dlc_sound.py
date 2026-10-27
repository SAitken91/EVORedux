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

Usage: python tools/dlc_sound.py <pack folder name> [<dlc_pc root>] [<PC build\\data>]
Writes <dlc_pc root>\\<pack>\\soundsystem\\win\\TrialsEvo_DLC<N>.fev and renames the .fsb banks to
.pak. Install by linking DLCContent\\<pack>\\soundsystem to that folder's soundsystem.
"""
import os
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
    if len(sys.argv) < 2 or sys.argv[1] not in PACKS:
        sys.exit(__doc__)
    main(*sys.argv[1:])
