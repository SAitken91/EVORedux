@echo off
rem Installs the Trials Evolution revival login shim. Run from this folder.
rem Usage: install.bat ["path\to\TrialsPC\datapack"]
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\Program Files (x86)\Steam\steamapps\common\TrialsPC\datapack"
if not exist "%GAME%\trialsFMX.exe" (
  echo Could not find trialsFMX.exe in "%GAME%".
  echo Run: install.bat "path\to\TrialsPC\datapack"
  exit /b 1
)
tasklist /fi "imagename eq trialsFMX.exe" | find /i "trialsFMX.exe" >nul && (
  echo Close Trials Evolution first.
  exit /b 1
)
if not exist "%GAME%\ubiorbitapi_r2_loader_orig.dll" (
  move "%GAME%\ubiorbitapi_r2_loader.dll" "%GAME%\ubiorbitapi_r2_loader_orig.dll" >nul || exit /b 1
)
copy /y "%~dp0ubiorbitapi_r2_loader.dll" "%GAME%\ubiorbitapi_r2_loader.dll" >nul || exit /b 1
echo Installed. The game now sends a random per-install key instead of your Ubisoft password.
echo Keep "%GAME%\trials_revive_login.key" (created on first login): it is your revival login.
