@echo off
rem Double-click launcher for propfix-test.ps1 (branch crowd-prop-cull-fix). The game window opens; quit the game
rem normally and the log is copied into runs\propfix automatically. Keep this window open until it says so.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0propfix-test.ps1" %*
pause
