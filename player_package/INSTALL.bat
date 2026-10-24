@echo off
if not exist "%~dp0install.ps1" goto notextracted
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
exit /b
:notextracted
echo.
echo  This file was opened from inside the zip, so the installer cannot run.
echo.
echo  1. Close this window and the zip window.
echo  2. Right-click TrialsEvolutionRevival.zip and choose "Extract All...", then "Extract".
echo  3. Open the new TrialsEvolutionRevival folder and run this file from there.
echo.
pause
exit /b 1
