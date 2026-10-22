# Timed autoplay baseline run of this worktree's build (build\release\hwder.exe), save-safe.
#
# What it does, in order:
#   1. refuses if hwder.exe is already running (one live game at a time);
#   2. points HWDER_NSP / HWDER_KEYS at the game files (keeps them where they are, never copies them);
#   3. backs up build\release\save to save.bak-<timestamp> and seeds ONLY zmha.bin / zmhasd.bin from the
#      fixed autoplay fixture in ..\test-saves\autoplay (read-only; the fixture is never written);
#   4. copies the sibling main checkout's hwder_settings.ini so the run uses the same display settings;
#   5. runs the game for -Seconds (default 150) with autoplay, filmstrip, drawlog and log under runs\<RunName>;
#   6. stops that exact process by PID and restores the backed-up save folder (the build keeps no autoplay save).
#
# Game path: HWDER_NSP / HWDER_KEYS are used when already set, otherwise the local Quiver install.
param(
    [string]$RunName = ('baseline-' + (git -C $PSScriptRoot rev-parse --short HEAD)),
    [int]$Seconds = 150,
    [string]$SettingsFrom = (Join-Path (Split-Path -Parent $PSScriptRoot) 'HWDER\build\release\hwder_settings.ini')
)
$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build\release'
$exe = Join-Path $build 'hwder.exe'
$game = 'C:\Programs\QuiverLauncher-win-Portable\Apps\HyruleWarriorsDefinitiveEdition-HyruleWarriorsDefinitiveEditionRecompHWDER'
$fixture = Join-Path (Split-Path -Parent $PSScriptRoot) 'test-saves\autoplay\0100AE00096EA000\48574445525F55530000000000000001'
$leaf = 'save\0100AE00096EA000\48574445525F55530000000000000001'
$runDir = Join-Path $PSScriptRoot ('runs\' + $RunName)

if (-not (Test-Path -LiteralPath $exe)) { throw "Missing $exe (build first: python tools\build.py)" }
$active = @(Get-CimInstance Win32_Process -Filter "Name = 'hwder.exe'")
if ($active.Count) { throw ('HWDER is already running (PID ' + ($active.ProcessId -join ', ') + '). Not starting a second one.') }

# 2. game files: keep an explicit HWDER_NSP/HWDER_KEYS, else the local install. Drop every other HWDER_* switch.
$nsp = $env:HWDER_NSP; $keys = $env:HWDER_KEYS
Get-ChildItem Env: | Where-Object Name -Like 'HWDER_*' | ForEach-Object { Remove-Item ('Env:' + $_.Name) }
if (-not $nsp) { $nsp = Join-Path $game 'Hyrule Warriors Definitive Edition.nsp' }
if (-not $keys) { $keys = Join-Path $game 'prod.keys' }
foreach ($p in $nsp, $keys) { if (-not (Test-Path -LiteralPath $p)) { throw "Missing game file $p" } }

# 3. save: back up the build's own save, seed the two fixture files only.
$saveDir = Join-Path $build 'save'
$backup = $null
if (Test-Path -LiteralPath $saveDir) {
    $backup = Join-Path $build ('save.bak-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Rename-Item -LiteralPath $saveDir -NewName (Split-Path -Leaf $backup)
    Write-Host "Backed up save to $backup"
}
$seedDir = Join-Path $build $leaf
New-Item -ItemType Directory -Force -Path $seedDir | Out-Null
foreach ($name in 'zmha.bin', 'zmhasd.bin') {
    $src = Join-Path $fixture $name
    if (-not (Test-Path -LiteralPath $src)) { throw "Missing fixture file $src" }
    Copy-Item -LiteralPath $src -Destination (Join-Path $seedDir $name)
}

try {
    # 4. settings: same display settings as the main build.
    if (Test-Path -LiteralPath $SettingsFrom) {
        Copy-Item -LiteralPath $SettingsFrom -Destination (Join-Path $build 'hwder_settings.ini')
    } else {
        Write-Warning "No settings file at $SettingsFrom; the build runs with its own hwder_settings.ini"
    }

    # 5. the run.
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $env:HWDER_NSP = $nsp; $env:HWDER_KEYS = $keys
    $env:HWDER_AUTOPLAY = '2'; $env:HWDER_AUTOPLAY_SKIP = '1'
    $env:HWDER_MUTE = '1'; $env:HWDER_NO_PRELAUNCH = '1'
    $env:HWDER_PIPELINE_CATALOG = '1'; $env:HWDER_PIPELINE_PREWARM = '1'
    $env:HWDER_FILMSTRIP = Join-Path $runDir 'filmstrip'
    $env:HWDER_DRAWLOG = '1'
    $env:HWDER_LOG = Join-Path $runDir 'hwder.log'
    Write-Host "Starting $exe for $Seconds s, output in $runDir"
    $proc = Start-Process -FilePath $exe -WorkingDirectory $build -PassThru
    $exited = $proc.WaitForExit($Seconds * 1000)
    if ($exited) {
        Write-Warning ("hwder.exe (PID {0}) exited on its own after {1:N0} s, exit code {2}" -f $proc.Id, ((Get-Date) - $proc.StartTime).TotalSeconds, $proc.ExitCode)
    } else {
        Write-Host "Stopping hwder.exe PID $($proc.Id)"
        Stop-Process -Id $proc.Id -Force
        $proc.WaitForExit(15000) | Out-Null
    }
} finally {
    # 6. put the build's own save back.
    if (Test-Path -LiteralPath $saveDir) { Remove-Item -LiteralPath $saveDir -Recurse -Force }
    if ($backup) { Rename-Item -LiteralPath $backup -NewName 'save'; Write-Host "Restored save from $backup" }
}
Write-Host "Done: $runDir"
