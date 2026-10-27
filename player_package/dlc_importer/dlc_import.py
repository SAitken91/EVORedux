"""Import your own Xbox 360 Trials Evolution DLC dump into Trials Evolution Gold (PC).

Takes the DLC package files dumped from your own console (the ~1 GB "LIVE" files for Origin of
Pain and/or Riders of Doom) and runs every conversion the PC game needs, in order:

  data.pak     stfs_extract -> dlc_convert -> dlc_vtex -> dlc_baked -> dlc_terrain -> dlc_foliage
               -> dlc_trackmagic -> dlc_fire_materials
  configs.pak  fixed leaderboard ids (the live server's boards) -> dlc_events -> dlc_environment
               -> dlc_items
  build\\data\\vtex  merged per-world object texture index (dlc_object_index), after install

Everything is built in a work folder first; the game's DLCContent folder is only replaced once a
pack has converted completely. Nothing is downloaded or uploaded.

Usage: python dlc_import.py [--game <TrialsPC folder>] [<package file or folder> ...]
           [--work <folder>] [--no-install] [--keep-work]
With no --game, the Steam and Ubisoft Connect installs are searched. With no files, the "DLC"
folder next to this tool's folder is searched for package files.
"""
import argparse
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stfs_extract                                    # noqa: E402
import dlc_convert                                     # noqa: E402
import dlc_vtex                                        # noqa: E402
import dlc_baked                                       # noqa: E402
import dlc_terrain                                     # noqa: E402
import dlc_foliage                                     # noqa: E402
import dlc_trackmagic                                  # noqa: E402
import dlc_fire_materials                              # noqa: E402
import dlc_leaderboards                                # noqa: E402
import dlc_events                                      # noqa: E402
import dlc_environment                                 # noqa: E402
import dlc_items                                       # noqa: E402
import dlc_unpack                                      # noqa: E402
import dlc_sound                                       # noqa: E402

PACKS = {"origin of pain": "OriginofPain", "riders of doom": "RidersofDoom"}
TOOLS = os.path.dirname(os.path.abspath(__file__))


def step(text):
    print(f"[{time.strftime('%H:%M:%S')}] {text}", flush=True)


def pack_of(package):
    pkg = stfs_extract.STFS(package)
    name = pkg.display_name.lower()
    for key, folder in PACKS.items():
        if key in name:
            return folder, pkg
    raise SystemExit(f"{package}: '{pkg.display_name}' is not a Trials Evolution DLC pack this tool knows")


def through(convert, path):
    """Run a SOURCE -> OUTPUT pak converter in place on path."""
    tmp = path + ".tmp"
    convert(path, tmp)
    os.replace(tmp, path)


def game_running():
    import subprocess
    out = subprocess.run(["tasklist", "/fi", "imagename eq trialsFMX.exe"], capture_output=True, text=True)
    return "trialsfmx.exe" in out.stdout.lower()


def find_game():
    """Same search as the EVO Redux installer: Steam libraries, then Ubisoft Connect installs."""
    import re
    import winreg

    def reg(root, key, value):
        for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
            try:
                with winreg.OpenKey(root, key, 0, winreg.KEY_READ | view) as k:
                    return winreg.QueryValueEx(k, value)[0]
            except OSError:
                pass
        return None

    roots = [reg(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
             reg(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath"),
             r"C:\Program Files (x86)\Steam"]
    candidates = []
    for r in filter(None, roots):
        libraries = [r]
        vdf = os.path.join(r, "steamapps", "libraryfolders.vdf")
        if os.path.exists(vdf):
            with open(vdf, encoding="utf-8", errors="replace") as f:
                libraries += [m.replace("\\\\", "\\") for m in re.findall(r'"path"\s+"([^"]+)"', f.read())]
        candidates += [os.path.join(lib, "steamapps", "common", "TrialsPC") for lib in libraries]
    installs = r"SOFTWARE\Ubisoft\Launcher\Installs"
    for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, installs, 0, winreg.KEY_READ | view) as k:
                for i in range(winreg.QueryInfoKey(k)[0]):
                    d = reg(winreg.HKEY_LOCAL_MACHINE, installs + "\\" + winreg.EnumKey(k, i), "InstallDir")
                    if d:
                        candidates.append(d.replace("/", "\\"))
        except OSError:
            pass
    for c in candidates:
        if os.path.exists(os.path.join(c, "datapack", "trialsFMX.exe")):
            return c
    return None


def find_packages(paths):
    """Package files among paths (files or folders): Xbox STFS files start with LIVE, CON or PIRS."""
    found = []
    for p in paths:
        files = [p] if os.path.isfile(p) else [os.path.join(dp, f) for dp, _, fs in os.walk(p) for f in fs]
        for f in files:
            try:
                with open(f, "rb") as h:
                    if h.read(4) in (b"LIVE", b"CON ", b"PIRS"):
                        found.append(f)
            except OSError:
                pass
    return found


def check_game(game):
    for rel in ("datapack/trialsFMX.exe", "build/data/data.pak"):
        if not os.path.exists(os.path.join(game, rel)):
            raise SystemExit(f"{game} doesn't look like the Trials Evolution Gold folder (no {rel})")


def convert_sound(folder, raw, pc, work, game):
    """Xbox XMA sound banks -> PC PCM16 banks (vgmstream decodes) + the pack's PC sound project."""
    import subprocess
    vgmstream = os.path.join(TOOLS, "audio", "vgmstream", "vgmstream-cli.exe")
    if not os.path.exists(vgmstream):
        step(f"{folder}: vgmstream not found, skipping sound (the DLC tracks will be silent)")
        return
    step(f"{folder}: converting sound banks (takes a few minutes)")
    unpacked = os.path.join(work, "unpacked_" + folder)
    dlc_unpack.unpack(os.path.join(raw, folder), os.path.join(unpacked, folder))
    subprocess.run([sys.executable, os.path.join(TOOLS, "dlc_audio.py"), "--source-root", unpacked,
                    "--output-root", pc, "--vgmstream", vgmstream,
                    "--work-dir", os.path.join(work, "audio_" + folder)],
                   check=True, stdout=subprocess.DEVNULL)
    dlc_sound.main(folder, pc, os.path.join(game, "build", "data"))
    sound = os.path.join(pc, folder, "soundsystem", "win")
    for f in os.listdir(sound):
        if f.endswith(".verification.json"):              # dlc_audio's per-bank check reports
            os.remove(os.path.join(sound, f))
    shutil.rmtree(unpacked, ignore_errors=True)


def convert_pack(folder, pkg, work, game):
    raw = os.path.join(work, "raw")
    pc = os.path.join(work, "pc")
    step(f"{folder}: checking the dump against its built-in hashes")
    bad = pkg.verify()
    if bad:
        raise SystemExit(f"{folder}: {len(bad)} damaged blocks in the package file. Copy it from the "
                         "console again (the copy is incomplete or corrupted).")
    step(f"{folder}: extracting the Xbox package")
    pkg.extract(os.path.join(raw, folder))
    step(f"{folder}: converting textures and archives")
    dlc_convert.convert_pack(os.path.join(raw, folder), os.path.join(pc, folder))
    step(f"{folder}: converting virtual textures (the slow part)")
    dlc_vtex.main(folder, raw, pc)
    step(f"{folder}: converting baked meshes")
    dlc_baked.main(folder, pc)
    data = os.path.join(pc, folder, "data", "data.pak")
    step(f"{folder}: converting terrain, foliage and track files")
    through(dlc_terrain.convert, data)
    through(dlc_foliage.convert, data)
    through(dlc_trackmagic.convert, data)
    step(f"{folder}: adding fire materials")
    dlc_fire_materials.main(game, pc)                  # reads the base game, edits the staged pack
    step(f"{folder}: events, weather and bikes")
    dlc_events.main(os.path.join(pc, folder))
    configs = os.path.join(pc, folder, "data", "configs.pak")
    through(dlc_environment.convert, configs)
    through(dlc_items.convert, configs)
    convert_sound(folder, raw, pc, work, game)
    for leftover in ("data.pak.before-vtex", "data.pak.before-fire-materials"):
        p = os.path.join(pc, folder, "data", leftover)
        if os.path.exists(p):
            os.remove(p)
    return os.path.join(pc, folder)


def install(folder, staged, game):
    target = os.path.join(game, "datapack", "DLCContent", folder)
    if os.path.exists(target):
        # Outside DLCContent: the game's DLC scan treats every folder in there as a pack, and a
        # stray one made it abort the scan (DLC vanished from the world map).
        old = os.path.join(game, "dlc_backup", folder)
        if os.path.exists(old):
            shutil.rmtree(old)
        os.makedirs(os.path.dirname(old), exist_ok=True)
        shutil.move(target, old)
        step(f"{folder}: previous install kept as {old}")
    os.makedirs(os.path.dirname(target), exist_ok=True)
    shutil.move(staged, target)
    step(f"{folder}: installed to {target}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", help="Trials Evolution Gold folder (the one holding datapack and build)")
    ap.add_argument("packages", nargs="*", help="DLC package files (or folders) dumped from your Xbox 360")
    ap.add_argument("--work", help="work folder (needs ~6 GB free; default: next to the game)")
    ap.add_argument("--no-install", action="store_true", help="convert only, leave the game untouched")
    ap.add_argument("--keep-work", action="store_true", help="don't delete the work folder afterwards")
    args = ap.parse_args()

    game = args.game or find_game()
    if not game:
        raise SystemExit('Couldn\'t find Trials Evolution Gold. Run again with --game "<game folder>".')
    game = os.path.abspath(game)
    check_game(game)
    step(f"game: {game}")
    packages = find_packages(args.packages or [os.path.join(os.path.dirname(TOOLS), "DLC")])
    if not packages:
        raise SystemExit("No DLC package files found. Put your dumped DLC files in the DLC folder "
                         "(or drag them onto IMPORT_DLC.bat) and run again.")
    packs = {}
    for package in packages:
        folder, pkg = pack_of(package)
        packs[folder] = pkg
        step(f"{package}: {pkg.display_name}")
    work = os.path.abspath(args.work or os.path.join(game, "dlc_import_work"))
    os.makedirs(work, exist_ok=True)

    import json
    with open(os.path.join(TOOLS, "dlc_leaderboard_ids.json")) as f:
        mapping = json.load(f)

    staged = {}
    for folder, pkg in packs.items():
        staged[folder] = convert_pack(folder, pkg, work, game)
    step("giving the DLC tracks their online leaderboard ids")
    dlc_leaderboards.apply_fixed(os.path.join(work, "pc"), mapping)
    step("opening the DLC events on the world map")
    dlc_events.unlock(os.path.join(work, "pc"))

    if args.no_install:
        step(f"done; converted packs are in {os.path.join(work, 'pc')}")
        return
    if game_running():
        raise SystemExit("Trials Evolution is running. Close it and run this again.")
    for folder, path in staged.items():
        install(folder, path, game)
    step("building the DLC texture index")
    sys.argv = ["dlc_object_index.py", game, os.path.join(game, "build", "data", "vtex")]
    import dlc_object_index
    dlc_object_index.main()
    if not args.keep_work:
        shutil.rmtree(work, ignore_errors=True)
    step("all done - start the game and look for the DLC events on the world map")


if __name__ == "__main__":
    main()
