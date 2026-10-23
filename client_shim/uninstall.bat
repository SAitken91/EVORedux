@echo off
rem Restores the original Ubisoft loader.
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\Program Files (x86)\Steam\steamapps\common\TrialsPC\datapack"
if not exist "%GAME%\ubiorbitapi_r2_loader_orig.dll" (
  echo The shim is not installed in "%GAME%".
  exit /b 1
)
move /y "%GAME%\ubiorbitapi_r2_loader_orig.dll" "%GAME%\ubiorbitapi_r2_loader.dll" >nul || exit /b 1
echo Original loader restored.
