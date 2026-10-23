@echo off
rem Builds the 64-bit xmemdec.dll used by tools/xmem.py (falls back to pure Python without it).
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /LD /W3 xmemdec.c /Fe:xmemdec.dll /link /NOLOGO || exit /b 1
del /q xmemdec.obj xmemdec.exp xmemdec.lib 2>nul
echo Built xmemdec.dll
