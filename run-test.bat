@echo off
rem Runs this worktree's build with no HWDER_NSP / HWDER_KEYS, so the startup screen shows the missing files.
rem Pick them with the buttons. The game and keys are in C:\Programs\QuiverLauncher-win-Portable\Apps\HyruleWarriorsDefinitiveEdition-HyruleWarriorsDefinitiveEditionRecompHWDER
rem Settings (including the picked paths) are saved in build\release\hwder_settings.ini, never the installed copy.
set HWDER_NSP=
set HWDER_KEYS=
rem Start from the newest save (installed game or a sibling worktree's build), unless this build's own save is as new; backs up first.
powershell -NoProfile -Command "$rel='save\0100AE00096EA000\48574445525F55530000000000000001'; $me=[IO.Path]::GetFullPath('%~dp0build\release'); $roots=@('C:\Programs\QuiverLauncher-win-Portable\Apps\HyruleWarriorsDefinitiveEdition-HyruleWarriorsDefinitiveEditionRecompHWDER') + (git -C '%~dp0.' worktree list --porcelain | Where-Object { $_ -like 'worktree *' } | ForEach-Object { [IO.Path]::GetFullPath(($_.Substring(9) -replace '/','\') + '\build\release') } | Where-Object { $_ -ne $me }); $new=$roots | ForEach-Object { Get-Item (Join-Path $_ \"$rel\zmha.bin\") -ErrorAction SilentlyContinue } | Sort-Object LastWriteTime -Descending | Select-Object -First 1; $own=Get-Item (Join-Path $me \"$rel\zmha.bin\") -ErrorAction SilentlyContinue; if ($new -and (-not $own -or $new.LastWriteTime -gt $own.LastWriteTime)) { if (Test-Path \"$me\save\") { Copy-Item \"$me\save\" (\"$me\save.bak-\" + (Get-Date -Format yyyyMMdd-HHmmss)) -Recurse }; New-Item -ItemType Directory -Force (Join-Path $me $rel) | Out-Null; Copy-Item (Join-Path $new.DirectoryName 'zmha.bin'),(Join-Path $new.DirectoryName 'zmhasd.bin') (Join-Path $me $rel) -Force; Write-Host ('Save copied from ' + $new.DirectoryName) } else { Write-Host 'Save: this build already has the newest' }"
cd /d "%~dp0build\release"
"%~dp0build\release\hwder.exe" %*
pause
