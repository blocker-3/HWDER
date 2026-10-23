# Test launcher for branch crowd-prop-cull-fix ("Keep props in big battles", runtime/src/game/stage_props.cpp).
# Game and keys stay in the Quiver folder (pointed at, never copied). The save is seeded once from hywar\GOLDEN_SAVE
# when this build has none. The option is on by default; turn it off in the menu (Graphics) to compare.
# After the game exits, hwder.log is copied to runs\propfix\<timestamp>.log.
# Unattended tests only (propfix-test.bat never passes it): -Autoplay 2 drives the menus into a battle and keeps fighting.
param([string]$Autoplay = '')
$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build\release'
$exe = Join-Path $build 'hwder.exe'
$game = 'C:\Programs\QuiverLauncher-win-Portable\Apps\HyruleWarriorsDefinitiveEdition-HyruleWarriorsDefinitiveEditionRecompHWDER'
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing $exe (build this worktree first: python tools\build.py)" }
$active = @(Get-CimInstance Win32_Process -Filter "Name = 'hwder.exe'")
if ($active.Count) { throw ('HWDER is already running (PID ' + ($active.ProcessId -join ', ') + '). Close it first.') }

Get-ChildItem Env: | Where-Object Name -Like 'HWDER_*' | ForEach-Object { Remove-Item ('Env:' + $_.Name) }
$env:HWDER_NSP = Join-Path $game 'Hyrule Warriors Definitive Edition.nsp'
$env:HWDER_KEYS = Join-Path $game 'prod.keys'
$env:HWDER_PIPELINE_CATALOG = '1'; $env:HWDER_PIPELINE_PREWARM = '1'
if ($Autoplay) { $env:HWDER_AUTOPLAY = $Autoplay }

& (Join-Path (Split-Path -Parent $PSScriptRoot) 'GOLDEN_SAVE\seed-golden-save.ps1') -BuildDir $build

$settings = Join-Path $build 'hwder_settings.ini'
if (-not (Test-Path -LiteralPath $settings)) {
    # First launch of this worktree: start from main's settings (paths and display options), never the keys.
    $mainIni = Join-Path (Split-Path -Parent $PSScriptRoot) 'HWDER\build\release\hwder_settings.ini'
    if (Test-Path -LiteralPath $mainIni) { Copy-Item -LiteralPath $mainIni -Destination $settings }
}

$runs = Join-Path $PSScriptRoot 'runs\propfix'
New-Item -ItemType Directory -Force -Path $runs | Out-Null
Write-Host "Starting HWDER (crowd-prop-cull-fix). Quit the game normally when done; the log is kept in $runs"
$p = Start-Process -FilePath $exe -WorkingDirectory $build -PassThru
$p.WaitForExit()
$stamp = Get-Date -Format yyyyMMdd-HHmmss
Copy-Item -LiteralPath (Join-Path $build 'hwder.log') -Destination (Join-Path $runs "$stamp.log")
Write-Host "Log saved: $runs\$stamp.log"
