@echo off
rem Starts the Trials Evolution revival server. Keep this window open while playing.
rem Close it (or press Ctrl+C) to stop the server.
title Trials Evolution revival server
cd /d "%~dp0"
python -u server.py
echo.
echo The server stopped. Press any key to close this window.
pause >nul
