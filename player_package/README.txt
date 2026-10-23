TRIALS EVOLUTION REVIVAL
========================

A fan-run replacement for the shut-down online servers of Trials Evolution: Gold Edition (PC).
Leaderboards, ghosts and Track Central boards work again, including thousands of archived
historical scores. Not affiliated with Ubisoft or RedLynx. You need to own the game on Steam.


INSTALL
-------
1. Close the game.
2. Unzip this folder anywhere and double-click INSTALL.bat.
3. Start Trials Evolution from Steam like normal. Ubisoft Connect still opens; that's expected.

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


UNINSTALL
---------
Double-click UNINSTALL.bat. The game goes back to its original files.


The game also shows the occasional revival message in the main-menu ticker.
If you enjoy the revival, you can help pay for the server at ko-fi.com/StewYork91 - thank you!
