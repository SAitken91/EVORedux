@echo off
rem EVO Redux - apply the latest DLC fixes to Xbox 360 DLC you already imported with IMPORT_DLC.bat.
rem No DLC files needed and nothing is converted again. Needs 64-bit Python 3.
setlocal
cd /d "%~dp0"
title EVO Redux - DLC update

if not exist "dlc_importer\dlc_import.py" goto :notextracted

set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY goto :nopython
%PY% -c "import struct,sys; sys.exit(0 if struct.calcsize('P') == 8 and sys.version_info >= (3, 10) else 1)" 2>nul || goto :nopython

echo EVO Redux DLC update
echo.
echo This applies the latest DLC fixes to the DLC you already imported with IMPORT_DLC.bat.
echo No DLC files needed. It takes a minute or two.
echo (Haven't imported your DLC yet? Run IMPORT_DLC.bat instead.)
echo Close Trials Evolution before continuing.
echo.
pause

%PY% -u "dlc_importer\dlc_import.py" --fixes-only %*
if errorlevel 1 (
    echo.
    echo The update did not finish. The message above says why.
    echo If you ask for help, send the file dlc_import_log.txt from this folder.
) else (
    echo.
    echo Done! The latest DLC fixes are installed.
)
echo.
pause
exit /b

:notextracted
echo The EVO Redux zip has to be extracted first.
echo.
echo Right-click the zip, choose "Extract All...", then open the extracted
echo TrialsEvolutionRevival folder and run UPDATE_DLC.bat from there.
echo.
pause
exit /b 1

:nopython
echo This needs 64-bit Python 3.10 or newer, which wasn't found.
echo.
echo 1. Download Python from https://www.python.org/downloads/
echo 2. Run the installer and tick "Add python.exe to PATH" on the first screen.
echo 3. Run UPDATE_DLC.bat again.
echo.
pause
