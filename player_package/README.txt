TRIALS EVOLUTION REVIVAL
========================

A fan-run replacement for the shut-down online servers of Trials Evolution: Gold Edition (PC).
Leaderboards, ghosts and Track Central boards work again, including thousands of archived
historical scores. Not affiliated with Ubisoft or RedLynx. You need to own the game (Steam or Ubisoft Connect).


INSTALL
-------
1. Close the game.
2. Unzip it first: right-click the zip -> Extract All -> Extract. Then open the extracted
   TrialsEvolutionRevival folder and double-click INSTALL.bat (it finds the game by itself).
3. Start Trials Evolution like normal (Steam or Ubisoft Connect). Ubisoft Connect still opens;
   that's expected.

If it says it can't write to the game folder, right-click INSTALL.bat -> Run as administrator.
If Steam ever "verifies game files", run INSTALL.bat again.


WHY THE INSTALLER IS NEEDED (please read)
-----------------------------------------
The game sends the password Ubisoft Connect gives it to the online server, and its built-in
encryption is too weak to protect it on the way. The installer adds a small file
(ubiorbitapi_r2_loader.dll) that swaps your Ubisoft password for a random revival key before it
ever leaves your PC. Ubisoft Connect, your saves and your ownership check are untouched.

The revival server only talks to games that have this file. A game without it (for example after
Steam "verifies game files") is simply offline, like before the revival, and never sends your
password anywhere. If online features stop working, run INSTALL.bat again.


YOUR REVIVAL LOGIN
------------------
The first time you play, the game creates this file:
    ...\steamapps\common\TrialsPC\datapack\trials_revive_login.key
It is your revival login: the first key used with your Ubisoft name claims that name on the
leaderboards. Back it up. If you reinstall, copy it back into the datapack folder.
Lost it? Contact the server admin to have your name released.


XBOX 360 DLC (OPTIONAL)
-----------------------
EVO Redux can run the Xbox 360 DLC packs (Origin of Pain, Riders of Doom) on PC. The DLC is not
included: you need the package files dumped from your own Xbox 360.
1. Install INSTALL.bat first (above).
2. Install 64-bit Python 3.10+ from https://www.python.org/downloads/ and tick
   "Add python.exe to PATH" on its first screen.
3. Put your DLC files in the DLC folder here (or drag them onto IMPORT_DLC.bat). The ~1 GB
   package files or packs already extracted to folders both work.
4. Close the game and double-click IMPORT_DLC.bat. It takes about 5 minutes and needs about
   7 GB of free space while it works.
If it fails: "Access denied" -> right-click IMPORT_DLC.bat -> Run as administrator. Running it
again is safe. Each run saves dlc_import_log.txt here; post it when asking for help.


UNINSTALL
---------
Double-click UNINSTALL.bat. The game goes back to its original files.


The game also shows the occasional revival message in the main-menu ticker.
If you enjoy the revival, you can help pay for the server at ko-fi.com/StewYork91 - thank you!
