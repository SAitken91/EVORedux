@echo off
rem Builds the 32-bit login shim with Visual Studio 2022 (C++ workload).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cl /nologo /O2 /MT /EHsc /W4 /LD orbit_shim.cpp ticker_hook.cpp gamelog_hook.cpp dlc_fixes.cpp game_build.cpp ugc_feeds.cpp /Fe:ubiorbitapi_r2_loader.dll /link /DEF:orbit_shim.def /NOLOGO || exit /b 1
del /q orbit_shim.obj ticker_hook.obj gamelog_hook.obj dlc_fixes.obj game_build.obj ugc_feeds.obj ubiorbitapi_r2_loader.exp ubiorbitapi_r2_loader.lib 2>nul
echo Built ubiorbitapi_r2_loader.dll
