# EVO Redux — Trials Evolution: Gold Edition, back online

Ubisoft shut down the online servers for **Trials Evolution: Gold Edition** (PC) years ago, and
with them went the leaderboards, the ghosts, Track Central and multiplayer. **EVO Redux** is a
fan-made replacement server that brings them back.

**The goal is for everything to work again** — leaderboards, ghosts, Track Central, multiplayer
and the rest of the online side, as it did on day one. We're not there yet, but the leaderboards
are live right now and waiting for times.

Not affiliated with Ubisoft or RedLynx. You need your own copy of the game (Steam or Ubisoft Connect).

---

## Thank you — and please spread the word

To everyone who has already installed this and put a time on the board: **thank you.** A
leaderboard is only as good as the people on it, and every run you upload brings these boards
a little closer to what they used to be.

If you enjoy it, **tell people.** Post it in your Discords, share it with your old Trials
friends, drop it in the Steam forums. The more riders come back, the more these great
leaderboards fill up again — let's bring the glory back to them.

---

## What works today

- ✅ **Online login** through the revival server
- ✅ **Global and per-track leaderboards** for every track in the game
- ✅ **Ghosts** — upload your best run, download and race other players' ghosts
- ✅ **Track Central is back in the game** — Get Tracks, search and download over 1,600 Xbox 360
  community tracks, with screenshots, ratings and the original creators (see
  [Get Track Central tracks](#get-track-central-tracks))
- ✅ **Track Central tracks you have installed** are playable, with leaderboards that include
  thousands of archived historical scores (see [Legacy spots](#legacy-spots-the-historical-scores))
- ✅ Occasional revival messages in the main-menu news ticker
- ✅ **Xbox 360 DLC packs run on PC** — Origin of Pain and Riders of Doom, including Neverending
  Trials, fire effects and their leaderboards (see [Xbox 360 DLC](#xbox-360-dlc))

## The plan

The aim is the full online experience, not just part of it:

- 🔜 **Multiplayer** — online races with other players
- 🔜 **Track Central in full** — uploading your own tracks, and the Xbox 360 tracks that use DLC
  objects (about 4,200 more)
- 🔜 Everything else the original servers did, one piece at a time

Progress is posted here as features come online.

---

## Legacy spots (the historical scores)

When the official servers were still up, the game kept a local copy of each Track Central
track's global leaderboard. We recovered those cached boards from a large archive of
preserved tracks and loaded them back in: **58,543 historical scores from 6,559 players.**

On Track Central boards these show up as players named **`Legacy#<number>`**:

- They are **real scores from the original servers**, recorded before the shutdown (the archive
  dates from December 2017).
- The game only stored each player's ID number, not their name, so the original riders appear
  by number.
- They are ranked **right alongside new runs.** Beat a legacy time and you take its place — some
  of these are seriously fast.
- Legacy entries can't be claimed or logged into; they're there as the benchmark to chase.

---

## Install (players)

**30-second video walkthrough** (click to watch):

[![How to download, extract and install](docs/install_video_preview.png)](docs/install_video.mp4)

1. **Close the game.**
2. Download the player package: `TrialsEvolutionRevival.zip` from the
   [Releases](../../releases) page — or click **Code → Download ZIP** here and open the
   `player_package` folder.
3. **Unzip it first** (right-click → *Extract All* → *Extract*), open the extracted
   `TrialsEvolutionRevival` folder and double-click **`INSTALL.bat`**. It finds the game by itself,
   whether you have it on Steam or Ubisoft Connect. (If it can't write to the game folder,
   right-click it → *Run as administrator*.)
4. Start Trials Evolution as usual. Ubisoft Connect still opens; that's expected.

If Steam or Ubisoft Connect ever **verifies game files**, the fix is removed — just run
`INSTALL.bat` again. To remove everything, run **`UNINSTALL.bat`**.

### Still offline?

- Make sure you have the **latest release** (v1.2 or newer). Older versions only worked on the
  Steam copy of the game, so Ubisoft Connect players stayed offline.
- Run `INSTALL.bat` again after downloading it, then start the game.
- Still offline? Send us `trials_revive_shim.log` from the game's `datapack` folder (on Steam:
  `...\steamapps\common\TrialsPC\datapack\`). It says exactly what went wrong.

### Your revival login

The first time you play, the game creates `trials_revive_login.key` in the game's `datapack`
folder (on Steam: `...\steamapps\common\TrialsPC\datapack\`). **Back it up.** The first key used with your Ubisoft
name claims that name on the boards; if you reinstall, copy the file back. If you lose it, ask
the server admin to release your name (your scores and ghosts stay attached to it).

---

## Get Track Central tracks

**In the game:** open Track Central → **Get Tracks**. Over 1,600 Xbox 360 community tracks are
there to browse (Most Downloaded, Highest Rated, New, RedLynx Picks) or search by track name or
creator. Download one and it's in your Play Tracks list. This needs EVO Redux v1.5.2 or newer.

Tip: download the tracks you want rather than installing thousands by hand — the Play Tracks page
loads faster with fewer tracks installed.

**The PC archive:** the community also has an archive of **about 700 of the best-rated PC Track
Central tracks**, shared by Scruffy McGuffy on the Steam forums:

**[Archive of Track Central tracks (Steam discussion)](https://steamcommunity.com/app/220160/discussions/0/1680315447969304289/)**

1. Download the archive from the link in that thread.
2. Extract it into `Documents\Ubisoft\Trials Evolution Gold Edition\SavedGames`.
3. Start the game: the tracks show up under Track Central.

With EVO Redux installed they're **playable with working leaderboards**, including the original
scores from before the shutdown ([Legacy spots](#legacy-spots-the-historical-scores)). Set a
time and you're on the board with them.

---

## Xbox 360 DLC

The PC Gold Edition never got the Xbox 360 DLC packs. EVO Redux makes them run on PC:
**Origin of Pain** and **Riders of Doom**, including Neverending Trials with its endless
generated rounds, the fire effects, and online leaderboards for every DLC track. It works with
both the **Steam** and the **Ubisoft Connect** version of the game.

EVO Redux does **not** include the DLC itself. The tracks and assets belong to Ubisoft/RedLynx
and are not distributed here. You need your own copy of the DLC packs, dumped from your own
Xbox 360 (the ~1 GB package files for Origin of Pain and/or Riders of Doom).

**Import your DLC (one-time, about 5 minutes):**

1. Install EVO Redux first (`INSTALL.bat`, see [Install](#install-players)).
2. Install [Python 3.10 or newer](https://www.python.org/downloads/) (64-bit) and tick
   **"Add python.exe to PATH"** on the installer's first screen.
3. Extract the EVO Redux zip (right-click → Extract All). Put your dumped DLC in its `DLC`
   folder, or drag it onto `IMPORT_DLC.bat`. Either the ~1 GB package files or packs you
   already extracted to folders work; file names and dump language don't matter.
4. Close the game and double-click **`IMPORT_DLC.bat`**.

The importer finds your game, checks the dump against the hashes stored inside it (a damaged
copy is reported instead of installed), converts everything the PC game needs (textures,
terrain, tracks, sound, leaderboard ids) and installs it to `datapack\DLCContent`. The DLC
events are open on the world map straight away. An earlier DLC install is kept in
`TrialsPC\dlc_backup`.

**If the import doesn't work:**

- It needs about **7 GB of free space** while it works: next to the game if that folder is
  writable, otherwise in `%LOCALAPPDATA%\EVORedux`. It tells you if a drive is too full.
- "Access denied": right-click `IMPORT_DLC.bat` → **Run as administrator**.
- "Python wasn't found": install 64-bit Python 3.10+ with "Add python.exe to PATH" ticked.
- Running it again is safe: it cleans up an earlier unfinished attempt first.
- Every run saves **`dlc_import_log.txt`** next to `IMPORT_DLC.bat`. If it still fails,
  post that file (or a screenshot of the window) when asking for help.

The importer is [`tools/dlc_import.py`](tools/dlc_import.py); it runs the individual
`dlc_*.py` conversion tools in order. Sound is decoded with
[vgmstream](https://github.com/vgmstream/vgmstream) (included, with its license).

---

## Your Ubisoft password stays on your PC

The game sends whatever password Ubisoft Connect hands it to the online server, and its built-in
network encryption uses a key that is publicly known — so that password would be readable by
anyone on the network path. That's why the installer exists:

- The installed file (`ubiorbitapi_r2_loader.dll`) sits in front of Ubisoft's loader and
  **replaces your password and CD key with a random revival key** before the game ever sees
  them. Your real password never leaves your PC.
- **The server only talks to games that have this file.** A game without it (for example after
  a Steam file verification) is simply offline, like before the revival, and never sends
  anything to the server.
- Ubisoft Connect, your saves and your ownership check are untouched. Everything else is
  forwarded to Ubisoft's original DLL unchanged.

All of this is open source in [`client_shim/`](client_shim) — read it, build it yourself if you
like (`client_shim/build.bat`, Visual Studio 2022 C++ workload).

---

## Tools

Most players only ever need the installer and `IMPORT_DLC.bat`. The rest is here for people who
want it.

**For players**

| Tool | What it does |
|---|---|
| `INSTALL.bat` (release zip) | Installs or updates EVO Redux. Finds Steam and Ubisoft Connect installs. |
| `IMPORT_DLC.bat` (release zip) | One-click import of the Xbox 360 DLC packs you own (see [Xbox 360 DLC](#xbox-360-dlc)). Run it again after an update to pick up DLC fixes. |
| `tools/switch_server.bat` | Switches the game between the live server and a local test server. |
| `tools/x360_tracks.py` | Converts a folder of Xbox 360 custom track packages to PC tracks (Python 3): `python tools/x360_tracks.py <360 tracks> <output>`. Most people don't need it: the same tracks are on Track Central. |
| `tools/x360_metadata.py`, `tools/x360_thumbnails.py` | Fix 360 tracks converted before v1.5.2 (wrong track ids, missing thumbnails): `python tools/x360_metadata.py <SavedGames>` and `python tools/x360_thumbnails.py <SavedGames>` (Windows). |
| `tools/dlc_cubemaps.py` | Repairs the shiny DLC surfaces (rainbow reflections) of a DLC imported before v1.5.2, without a full re-import. |

**For server hosts**

| Tool | What it does |
|---|---|
| `tools/build_track_library.py` | Builds `track_library.db` (the in-game Track Central library) from tracks converted by `x360_tracks.py`. The server loads it at start; without it Get Tracks is empty. |
| `tools/rename_player.py` | Sets the name shown for a player on the boards. |
| `tools/import_history.py`, `tools/gen_boards.py` | Import the archived historical scores; generate the leaderboard sort orders. |
| `tools/dlc_*.py`, `tools/pak.py`, `tools/stfs_extract.py` | The DLC conversion steps the importer runs, and pak / Xbox package utilities. |

---

## For developers

| Path | What it is |
|---|---|
| `server.py`, `prudp.py`, `quazal.py` | The server: Quazal PRUDP transport + the RendezVous services the game uses (auth, secure, leaderboards, ghosts/user storage, Track Central/UGC) and the HTTP online-config endpoint |
| `boards.json` | Leaderboard sort orders, generated from the game's own track list by `tools/gen_boards.py` |
| `client_shim/` | Source of the player DLL (login protection, server gate, menu ticker) |
| `player_package/` | What players install |
| `track_library.py` | The in-game Track Central library (feeds, search, downloads) served from `track_library.db` |
| `tools/` | See [Tools](#tools) |

Run a local server with `start_server.bat` (Python 3; set `TRIALS_HOST` to the address players
connect to). The server creates its own `server_secret.key` and `trials.db` on first start —
never share or commit those.

---

## Support

The server costs real money to run. If you'd like to help keep it online:
**[ko-fi.com/StewYork91](https://ko-fi.com/StewYork91)** — thank you!

*Trials Evolution is a trademark of Ubisoft Entertainment. This project contains no Ubisoft or
RedLynx game files.*
