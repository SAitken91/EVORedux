@echo off
rem EVO Redux - import your own Xbox 360 Trials Evolution DLC dump into the PC game.
rem Put your dumped DLC package files in the DLC folder next to this file (or drag them onto
rem this file) and run it. Needs 64-bit Python 3 (https://www.python.org/downloads/).
setlocal
cd /d "%~dp0"
title EVO Redux - DLC importer

set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY goto :nopython
%PY% -c "import struct,sys; sys.exit(0 if struct.calcsize('P') == 8 and sys.version_info >= (3, 10) else 1)" 2>nul || goto :nopython

echo EVO Redux DLC importer
echo.
echo This converts the DLC files you dumped from your own Xbox 360 so the PC game can play them.
echo It takes 10-30 minutes and needs about 6 GB of free space next to the game.
echo Close Trials Evolution before continuing.
echo.
pause

%PY% -u "dlc_importer\dlc_import.py" %*
if errorlevel 1 (
    echo.
    echo The import did not finish. The message above says why.
    echo If you ask for help, include the text above.
) else (
    echo.
    echo Done! Start Trials Evolution and look for the DLC on the world map.
)
echo.
pause
exit /b

:nopython
echo This needs 64-bit Python 3.10 or newer, which wasn't found.
echo.
echo 1. Download Python from https://www.python.org/downloads/
echo 2. Run the installer and tick "Add python.exe to PATH" on the first screen.
echo 3. Run IMPORT_DLC.bat again.
echo.
pause
