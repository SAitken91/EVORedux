# Trials Evolution revival - player installer.
# Installs the login fix DLL and points the game at the revival server. Run via INSTALL.bat.
param([switch]$Uninstall)

$ErrorActionPreference = "Stop"
$ServerHost = "40.160.100.239"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Find-Game {
    $roots = @()
    foreach ($key in "HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam") {
        try {
            $p = (Get-ItemProperty $key -ErrorAction Stop)
            if ($p.SteamPath) { $roots += $p.SteamPath }
            if ($p.InstallPath) { $roots += $p.InstallPath }
        } catch {}
    }
    $roots += "C:\Program Files (x86)\Steam"
    $libraries = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        $libraries += $r
        $vdf = Join-Path $r "steamapps\libraryfolders.vdf"
        if (Test-Path -LiteralPath $vdf) {
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries += $m.Groups[1].Value -replace '\\\\', '\'
            }
        }
    }
    foreach ($lib in ($libraries | Select-Object -Unique)) {
        $game = Join-Path $lib "steamapps\common\TrialsPC"
        if (Test-Path -LiteralPath (Join-Path $game "datapack\trialsFMX.exe")) { return $game }
    }
    return $null
}

function Pause-Exit($code) {
    Write-Host ""
    Read-Host "Press Enter to close"
    exit $code
}

$game = Find-Game
if (-not $game) {
    Write-Host "Could not find Trials Evolution Gold Edition in your Steam libraries." -ForegroundColor Red
    $game = Read-Host "Paste the path to the TrialsPC folder (or press Enter to cancel)"
    if (-not $game -or -not (Test-Path -LiteralPath (Join-Path $game "datapack\trialsFMX.exe"))) { Pause-Exit 1 }
}
$datapack = Join-Path $game "datapack"
Write-Host "Game folder: $game"

if (Get-Process trialsFMX -ErrorAction SilentlyContinue) {
    Write-Host "Please close Trials Evolution first, then run this again." -ForegroundColor Yellow
    Pause-Exit 1
}

$loader = Join-Path $datapack "ubiorbitapi_r2_loader.dll"
$orig = Join-Path $datapack "ubiorbitapi_r2_loader_orig.dll"
$hostFiles = @((Join-Path $game "OnlineConfigHost.txt"), (Join-Path $datapack "OnlineConfigHost.txt"))

try {
    if ($Uninstall) {
        if (Test-Path -LiteralPath $orig) { Move-Item -Force -LiteralPath $orig -Destination $loader }
        foreach ($f in $hostFiles) {
            if (Test-Path -LiteralPath "$f.before-revival") { Move-Item -Force -LiteralPath "$f.before-revival" -Destination $f }
            elseif (Test-Path -LiteralPath $f) { Remove-Item -Force -LiteralPath $f }
        }
        Write-Host "Removed. The game is back to its original files." -ForegroundColor Green
        Write-Host "Your trials_revive_login.key was kept in the datapack folder in case you come back."
        Pause-Exit 0
    }

    if (-not (Test-Path -LiteralPath $orig)) { Move-Item -LiteralPath $loader -Destination $orig }       # keep Ubisoft's original once
    $shim = Join-Path $Here "ubiorbitapi_r2_loader.dll"
    Copy-Item -Force -LiteralPath $shim -Destination $loader
    if ((Get-FileHash -LiteralPath $shim).Hash -ne (Get-FileHash -LiteralPath $loader).Hash) {
        throw "the login fix DLL did not copy into the game folder"
    }
    foreach ($f in $hostFiles) {
        if ((Test-Path -LiteralPath $f) -and -not (Test-Path -LiteralPath "$f.before-revival")) { Copy-Item -LiteralPath $f -Destination "$f.before-revival" }
        [IO.File]::WriteAllText($f, $ServerHost)
    }
} catch {
    Write-Host "Install failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Try right-clicking INSTALL.bat and choosing 'Run as administrator'."
    Pause-Exit 1
}

Write-Host ""
Write-Host "Installed! Start Trials Evolution from Steam as usual." -ForegroundColor Green
Write-Host "Your game will log in with a private revival key instead of your Ubisoft password."
Write-Host "Back up this file - it is your revival login:" -ForegroundColor Yellow
Write-Host "  $datapack\trials_revive_login.key  (created the first time you start the game)"
Write-Host "If Steam 'verifies game files', just run INSTALL.bat again."
Pause-Exit 0
