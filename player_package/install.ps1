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
    $candidates = @()
    foreach ($lib in ($libraries | Select-Object -Unique)) { $candidates += Join-Path $lib "steamapps\common\TrialsPC" }

    # Ubisoft Connect: every game it has installed, plus its games folders.
    foreach ($key in "HKLM:\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs", "HKLM:\SOFTWARE\Ubisoft\Launcher\Installs") {
        try {
            foreach ($k in (Get-ChildItem $key -ErrorAction Stop)) {
                $dir = (Get-ItemProperty $k.PSPath -ErrorAction SilentlyContinue).InstallDir
                if ($dir) { $candidates += ($dir -replace '/', '\') }
            }
        } catch {}
    }
    $ubiRoots = @()
    foreach ($key in "HKLM:\SOFTWARE\WOW6432Node\Ubisoft\Launcher", "HKLM:\SOFTWARE\Ubisoft\Launcher") {
        try { $ubiRoots += (Get-ItemProperty $key -ErrorAction Stop).InstallDir } catch {}
    }
    $ubiRoots += "C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher", "C:\Program Files\Ubisoft\Ubisoft Game Launcher"
    foreach ($u in ($ubiRoots | Where-Object { $_ } | Select-Object -Unique)) {
        $games = Join-Path $u "games"
        if (Test-Path -LiteralPath $games) {
            foreach ($d in (Get-ChildItem -LiteralPath $games -Directory -ErrorAction SilentlyContinue)) { $candidates += $d.FullName }
        }
    }

    foreach ($c in ($candidates | Select-Object -Unique)) {
        $game = Test-GameDir $c
        if ($game) { return $game }
    }
    return $null
}

# The game folder is the one holding datapack\trialsFMX.exe (Steam: ...\steamapps\common\TrialsPC).
# Also accepts the datapack folder itself.
function Test-GameDir($dir) {
    if (-not $dir) { return $null }
    try {
        if (Test-Path -LiteralPath (Join-Path $dir "datapack\trialsFMX.exe")) { return $dir }
        if (Test-Path -LiteralPath (Join-Path $dir "trialsFMX.exe")) { return (Split-Path -Parent $dir) }
    } catch {}
    return $null
}

function Pause-Exit($code) {
    Write-Host ""
    Read-Host "Press Enter to close"
    exit $code
}

$game = Find-Game
if (-not $game) {
    Write-Host "Could not find Trials Evolution Gold Edition (Steam or Ubisoft Connect)." -ForegroundColor Red
    Write-Host "Paste the game's install folder - the one that contains datapack\trialsFMX.exe."
    $game = Test-GameDir ((Read-Host "Game folder (or press Enter to cancel)").Trim('"', ' '))
    if (-not $game) {
        Write-Host "trialsFMX.exe was not found there." -ForegroundColor Red
        Pause-Exit 1
    }
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
# Opt-in markers for the DLC runtime fixes (portals, vector logic); they only act on DLC tracks.
$dlcMarkers = @((Join-Path $datapack "trials_revive_portals.txt"), (Join-Path $datapack "trials_revive_vector_runtime.txt"))

try {
    if ($Uninstall) {
        if (Test-Path -LiteralPath $orig) { Move-Item -Force -LiteralPath $orig -Destination $loader }
        foreach ($f in $hostFiles) {
            if (Test-Path -LiteralPath "$f.before-revival") { Move-Item -Force -LiteralPath "$f.before-revival" -Destination $f }
            elseif (Test-Path -LiteralPath $f) { Remove-Item -Force -LiteralPath $f }
        }
        foreach ($m in $dlcMarkers) { if (Test-Path -LiteralPath $m) { Remove-Item -Force -LiteralPath $m } }
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
    foreach ($m in $dlcMarkers) { [IO.File]::WriteAllText($m, "on") }
} catch {
    Write-Host "Install failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Try right-clicking INSTALL.bat and choosing 'Run as administrator'."
    Pause-Exit 1
}

Write-Host ""
Write-Host "Installed! Start Trials Evolution as usual (Steam or Ubisoft Connect)." -ForegroundColor Green
Write-Host "Your game will log in with a private revival key instead of your Ubisoft password."
Write-Host "Back up this file - it is your revival login:" -ForegroundColor Yellow
Write-Host "  $datapack\trials_revive_login.key  (created the first time you start the game)"
Write-Host "If Steam or Ubisoft Connect 'verifies game files', just run INSTALL.bat again."
Write-Host "If the game still says offline, send us this file:" -ForegroundColor Yellow
Write-Host "  $datapack\trials_revive_shim.log"
Pause-Exit 0
