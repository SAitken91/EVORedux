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
import subprocess
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


def pack_in_configs(root):
    """Pack folder name from an extracted pack's data/configs.pak (pack1/... or pack2/... files)."""
    from pak import read_pak
    d, files = read_pak(os.path.join(root, "data", "configs.pak"))
    names = [n for n, _ in dlc_unpack.names_of(d, files)]
    for n in names:
        if n.startswith("pack1/"):
            return "OriginofPain"
        if n.startswith("pack2/"):
            return "RidersofDoom"
    return None


def pack_of(source, work, index):
    """(pack folder name, source) for a package file or an already-extracted pack folder.
    A package whose title isn't recognised (e.g. another language) is extracted and identified
    by its contents; the source then becomes the extracted folder."""
    if os.path.isdir(source):
        folder = pack_in_configs(source)
        if not folder:
            raise SystemExit(f"{source}: this folder isn't a Trials Evolution DLC pack")
        return folder, source
    pkg = stfs_extract.STFS(source)
    name = pkg.display_name.lower()
    for key, folder in PACKS.items():
        if key in name:
            return folder, pkg
    check_package(pkg, source)
    out = os.path.join(work, "raw", f"_package{index}")
    step(f"{source}: '{pkg.display_name}' - extracting to find out which pack it is")
    pkg.extract(out)
    folder = pack_in_configs(out) if os.path.exists(os.path.join(out, "data", "configs.pak")) else None
    if not folder:
        raise SystemExit(f"{source}: '{pkg.display_name}' is not a Trials Evolution DLC pack")
    return folder, out


def check_package(pkg, source):
    bad = pkg.verify()
    if bad:
        raise SystemExit(f"{source}: {len(bad)} damaged blocks in this package file. Copy it from the "
                         "console again (the copy is incomplete or corrupted).")


def through(convert, path):
    """Run a SOURCE -> OUTPUT pak converter in place on path."""
    tmp = path + ".tmp"
    convert(path, tmp)
    os.replace(tmp, path)


def game_running():
    import subprocess
    # Bytes, not text: tasklist prints its headers in the console code page (e.g. German umlauts),
    # which Python 3.15's UTF-8 default can't decode. The process name itself is plain ASCII.
    out = subprocess.run(["tasklist", "/fi", "imagename eq trialsFMX.exe"], capture_output=True)
    return b"trialsfmx.exe" in (out.stdout or b"").lower()


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
    """Package files among paths (files or folders): Xbox STFS files start with LIVE, CON or PIRS.
    A folder holding an already-extracted pack (data/configs.pak) counts as one package."""
    found = []
    for p in paths:
        if os.path.isdir(p):
            extracted = [dp for dp, _, fs in os.walk(p)
                         if os.path.basename(dp).lower() == "data" and "configs.pak" in [f.lower() for f in fs]]
            found += [os.path.dirname(dp) for dp in extracted]
        files = [p] if os.path.isfile(p) else [os.path.join(dp, f) for dp, _, fs in os.walk(p) for f in fs]
        for f in files:
            try:
                with open(f, "rb") as h:
                    if h.read(4) in (b"LIVE", b"CON ", b"PIRS"):
                        found.append(f)
            except OSError:
                pass
    return found


NEEDED_SPACE = 7 * 1024 ** 3      # extracted (~2 GB) + converted (~3 GB) packs, with headroom


def pick_work_folder(requested, game):
    """A writable work folder with enough free space: next to the game if possible (the install
    is then a quick move on the same drive), else in the user's local app data."""
    choices = [requested] if requested else [
        os.path.join(game, "dlc_import_work"),
        os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "EVORedux", "dlc_import_work")]
    problems = []
    for choice in choices:
        choice = os.path.abspath(choice)
        try:
            os.makedirs(choice, exist_ok=True)
            probe = os.path.join(choice, "write_test.tmp")
            with open(probe, "w") as f:
                f.write("ok")
            os.remove(probe)
        except OSError as e:
            problems.append(f"{choice}: not writable ({e.strerror})")
            continue
        free = shutil.disk_usage(choice).free
        if free < NEEDED_SPACE:
            problems.append(f"{choice}: only {free / 1024 ** 3:.1f} GB free, about 7 GB needed")
            continue
        return choice
    raise SystemExit("No work folder available:\n  " + "\n  ".join(problems)
                     + "\nFree up disk space, or run with --work \"<folder on a drive with space>\".")


SUPPORTED_BUILDS = {0x529D7E1B: "Steam", 0x52785567: "Ubisoft Connect"}   # trialsFMX.exe PE timestamps


def game_build(game):
    import struct
    with open(os.path.join(game, "datapack", "trialsFMX.exe"), "rb") as f:
        head = f.read(4096)
    pe = struct.unpack_from("<I", head, 0x3C)[0]
    return struct.unpack_from("<I", head, pe + 8)[0]


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


def convert_pack(folder, source, work, game):
    raw = os.path.join(work, "raw")
    pc = os.path.join(work, "pc")
    target = os.path.join(raw, folder)
    if isinstance(source, stfs_extract.STFS):
        step(f"{folder}: checking the dump against its built-in hashes")
        check_package(source, folder)
        step(f"{folder}: extracting the Xbox package")
        source.extract(target)
    elif os.path.abspath(source) != os.path.abspath(target):
        step(f"{folder}: copying the extracted pack")
        if os.path.dirname(os.path.abspath(source)) == os.path.abspath(raw):
            os.replace(source, target)                   # identified by contents, already ours
        else:
            shutil.copytree(source, target)
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


def replace_pak(pak, build):
    """build(source, output) -> True when it wrote output; swaps it in place of pak."""
    tmp = pak + ".tmp"
    try:
        if build(pak, tmp):
            os.replace(tmp, pak)
            return True
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return False


def restore_cut_items(game):
    configs = os.path.join(game, "datapack", "DLCContent", "OriginofPain", "data", "configs.pak")
    if not os.path.exists(configs):
        return
    step("restoring the cut Origin of Pain riding shorts")
    import dlc_restore_cut_items
    replace_pak(configs, dlc_restore_cut_items.main)


def have_numpy():
    try:
        import numpy  # noqa: F401
        return True
    except ImportError:
        pass
    step("installing numpy for the DLC ground (python -m pip install --user numpy)")
    if subprocess.run([sys.executable, "-m", "pip", "install", "--user", "numpy"]).returncode:
        return False
    import importlib
    import site
    site.addsitedir(site.getusersitepackages())
    importlib.invalidate_caches()
    try:
        import numpy  # noqa: F401
        return True
    except ImportError:
        return False


def paint_dlc_ground(game):
    """The 360 base ground layer, baked into the DLC terrain decals (dlc_terrain_base), then its
    materials taken out of the tiled atlas so the PC keeps their textures (dlc_untile_terrain).
    Bake first: it reads the layer order from the tiledAtlas values the second step clears."""
    if not have_numpy():
        step("numpy is not available: skipped the DLC base ground layer (the DLC ground stays plain). "
             "Install numpy (python -m pip install numpy) and run the importer again to add it.")
        return
    step("painting the DLC worlds' base ground layer")
    import dlc_terrain_base
    dlc_terrain_base.main(game)
    import dlc_untile_terrain
    for pack, world in (("OriginofPain", 1), ("RidersofDoom", 2)):
        pak = os.path.join(game, "datapack", "DLCContent", pack, "data", "data.pak")
        if not os.path.exists(pak):
            continue
        step(f"{pack}: base ground layer materials use the decal atlas")
        try:
            replace_pak(pak, lambda src, out: dlc_untile_terrain.main(src, out, world) or True)
        except SystemExit as e:                         # already untiled
            step(f"{pack}: {e}")


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
    build = game_build(game)
    if build not in SUPPORTED_BUILDS:
        raise SystemExit(
            f"This trialsFMX.exe ({build:08x}) is a game build the DLC fixes don't know (they support "
            "the Steam and Ubisoft Connect versions). Importing would not make the DLC playable. "
            "Please post dlc_import_log.txt so support for this version can be added.")
    step(f"game build: {SUPPORTED_BUILDS[build]}")
    packages = find_packages(args.packages or [os.path.join(os.path.dirname(TOOLS), "DLC")])
    if not packages:
        raise SystemExit("No DLC package files found. Put your dumped DLC files in the DLC folder "
                         "(or drag them onto IMPORT_DLC.bat) and run again.")
    work = pick_work_folder(args.work, game)
    step(f"work folder: {work}")
    for leftover in os.listdir(work):                     # only what an earlier, unfinished run made
        if leftover in ("raw", "pc") or leftover.startswith(("unpacked_", "audio_")):
            step(f"removing leftovers of an earlier run: {leftover}")
            shutil.rmtree(os.path.join(work, leftover), ignore_errors=True)
    packs = {}
    for i, package in enumerate(packages):
        folder, source = pack_of(package, work, i)
        if folder in packs:
            step(f"{package}: another copy of {folder}, skipped")
            continue
        packs[folder] = source
        step(f"{package}: {folder}")

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
    step("keeping Gold Edition bonus gear separate from the Xbox texture atlas")
    from pathlib import Path
    import dlc_pc_gear
    gear_stage = Path(work) / "pc_gear"
    dlc_pc_gear.stage(Path(game), gear_stage)
    if game_running():
        raise SystemExit("Trials Evolution is running. Close it before finishing the texture repair.")
    dlc_pc_gear.install(Path(game), gear_stage)
    restore_cut_items(game)
    paint_dlc_ground(game)
    if not args.keep_work:
        shutil.rmtree(work, ignore_errors=True)
    step("all done - start the game and look for the DLC events on the world map")


class Tee:
    """Copies console output into the log file players can send when asking for help."""
    def __init__(self, stream, log):
        self.stream, self.log = stream, log

    def write(self, text):
        self.stream.write(text)
        self.log.write(text)
        self.log.flush()

    def flush(self):
        self.stream.flush()


def run():
    log_path = os.path.join(os.path.dirname(TOOLS), "dlc_import_log.txt")
    try:
        log = open(log_path, "w", encoding="utf-8")
        sys.stdout = Tee(sys.stdout, log)
        sys.stderr = Tee(sys.stderr, log)
    except OSError:
        log_path = None
    step(f"EVO Redux DLC importer, Python {sys.version.split()[0]}, {sys.platform}")
    try:
        main()
    except SystemExit as e:
        if e.code not in (None, 0):
            print(f"\nERROR: {e.code}")
            if log_path:
                print(f"(details saved in {log_path})")
            sys.exit(1)
        raise
    except PermissionError as e:
        import traceback
        traceback.print_exc()
        print(f"\nERROR: Windows refused access to {e.filename}.\n"
              "Close the game, then right-click IMPORT_DLC.bat and choose 'Run as administrator'.")
        if log_path:
            print(f"(details saved in {log_path})")
        sys.exit(1)
    except Exception:
        import traceback
        traceback.print_exc()
        print("\nERROR: the import stopped unexpectedly (see above).")
        if log_path:
            print(f"Please send {log_path} when asking for help.")
        sys.exit(1)


if __name__ == "__main__":
    run()
