@echo off
rem Points Trials Evolution Gold at the live server or the local test server.
rem Usage: switch_server.bat [live^|test]   (no argument = flip to the other one)
rem Writes OnlineConfigHost.txt in both TrialsPC\ and TrialsPC\datapack\. Close the game first.
setlocal
set "GAME=C:\Program Files (x86)\Steam\steamapps\common\TrialsPC"
set "LIVE=40.160.100.239"
set "TEST=127.0.0.1"

if not exist "%GAME%\OnlineConfigHost.txt" (
    echo Could not find "%GAME%\OnlineConfigHost.txt". Edit GAME at the top of this file.
    goto :end
)
tasklist /fi "imagename eq trialsFMX.exe" | "%SystemRoot%\System32\find.exe" /i "trialsFMX.exe" >nul && (
    echo Trials is running. Close the game first, then run this again.
    goto :end
)

set /p CURRENT=<"%GAME%\OnlineConfigHost.txt"
set "MODE=%~1"
if "%MODE%"=="" (
    if "%CURRENT%"=="%TEST%" (set "MODE=live") else (set "MODE=test")
)
if /i "%MODE%"=="live" (set "HOST=%LIVE%") else if /i "%MODE%"=="test" (set "HOST=%TEST%") else (
    echo Unknown mode "%MODE%". Use live or test.
    goto :end
)

rem No trailing space or newline: the game reads the whole file as the host name.
rem (set /p always sets errorlevel 1, so check by reading the files back.)
<nul set /p "=%HOST%" > "%GAME%\OnlineConfigHost.txt" 2>nul
<nul set /p "=%HOST%" > "%GAME%\datapack\OnlineConfigHost.txt" 2>nul
set "CHECK1=" & set "CHECK2="
set /p CHECK1=<"%GAME%\OnlineConfigHost.txt"
set /p CHECK2=<"%GAME%\datapack\OnlineConfigHost.txt"
if not "%CHECK1%"=="%HOST%" goto :denied
if not "%CHECK2%"=="%HOST%" goto :denied

if /i "%MODE%"=="live" (
    echo Now on the LIVE server ^(%HOST%^). Play as normal.
) else (
    echo Now on the local TEST server ^(%HOST%^). Start the local server before launching the game.
)
goto :end

:denied
echo Could not write the file. Right-click this script and choose "Run as administrator".

:end
echo.
pause
