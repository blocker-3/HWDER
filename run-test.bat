@echo off
rem Runs this worktree's build with no HWDER_NSP / HWDER_KEYS, so the startup screen shows the missing files.
rem Pick them with the buttons. The game and keys are in C:\Programs\QuiverLauncher-win-Portable\Apps\HyruleWarriorsDefinitiveEdition-HyruleWarriorsDefinitiveEditionRecompHWDER
rem Settings (including the picked paths) are saved in build\release\hwder_settings.ini, never the installed copy.
set HWDER_NSP=
set HWDER_KEYS=
cd /d "%~dp0build\release"
"%~dp0build\release\hwder.exe" %*
pause
