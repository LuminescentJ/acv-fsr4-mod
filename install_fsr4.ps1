<#
 ACV FSR 4 mod - install / uninstall.

 Builds the release DLL (FSR 4 path only, no probe diagnostics) and installs it into the Valhalla folder together with
 AMD's FidelityFX loader + upscaler DLLs. Uninstall removes exactly what was installed (checked by SHA-256 against the
 manifest acv_fsr4_install.txt in the game folder) and restores a parked dxgi.dll.

 Usage (from the project folder, in PowerShell, with Valhalla closed):
     powershell -ExecutionPolicy Bypass -File .\install_fsr4.ps1              # build + install
     powershell -ExecutionPolicy Bypass -File .\install_fsr4.ps1 -Uninstall   # remove
 Optional:  -GameDir "D:\Games\Assassin's Creed Valhalla"   -NoBuild (install the last build)

 In game: FSR in Valhalla's graphics settings ON (e.g. Quality) = FSR 4 upscaling; FSR OFF = FSR 4 native AA.
 Keys: F2 = settings menu (Up/Down choose, Left/Right or Enter change, F2 or CLOSE to hide) for everything below,
       plus JITTER PHASES 8 (game default) / 16 / 32: more camera jitter positions for FSR 4 to combine (menu only).
       F7 = FSR 4 on/off (stock game when off), F4 = skip the game's own TAA + FSR 1 work on/off (on = faster),
       F8 / F9 = FSR 4 sharpness down / up (0.0 - 1.0, default 0.3),
       F3 = indicator square always on/off (top-left: green = FSR 4 upscaling, cyan = FSR 4 native AA; it also shows
       for 3 s after start-up, F7 and a mode change - no square = stock game).
 Every key shows a short on-screen message (e.g. "FSR 4 ON - UPSCALING", "FSR 4 OFF - GAME DEFAULT").
 Sharpness, F4 and F3 (and the same settings changed in the F2 menu) are remembered in %LOCALAPPDATA%\ACVFSR4\settings.ini.
 Log: %LOCALAPPDATA%\ACVFSR4\acv_fsr4.log
#>
param(
    [string]$GameDir = "",
    [switch]$Uninstall,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$root      = $PSScriptRoot
$buildDir  = Join-Path $root "build_release"
$builtDll  = Join-Path $buildDir "release\bin\Release\d3d12.dll"
$ffxBinDir = Join-Path $root "third_party\FidelityFX-SDK\Kits\FidelityFX\signedbin"
$ffxDlls   = @("amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll")

function Find-GameDir {
    $libs = @()
    $steam = $null
    try { $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath } catch {}
    foreach ($s in @($steam, "C:\Program Files (x86)\Steam", "C:\Program Files\Steam")) {
        if (-not $s) { continue }
        $s = $s -replace "/", "\"
        $libs += $s
        $vdf = Join-Path $s "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace "\\\\", "\")
            }
        }
    }
    foreach ($lib in ($libs | Select-Object -Unique)) {
        $candidate = Join-Path $lib "steamapps\common\Assassin's Creed Valhalla"
        if (Test-Path (Join-Path $candidate "ACValhalla.exe")) { return $candidate }
    }
    return $null
}

if (-not $GameDir) { $GameDir = Find-GameDir }
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir "ACValhalla.exe"))) {
    throw "Could not find ACValhalla.exe. Re-run with -GameDir ""<path to the game folder>""."
}
if (Get-Process ACValhalla -ErrorAction SilentlyContinue) { throw "Valhalla is running. Close it first." }
Write-Host "Game folder: $GameDir"

$manifest   = Join-Path $GameDir "acv_fsr4_install.txt"
$gameDxgi   = Join-Path $GameDir "dxgi.dll"
$dxgiParked = Join-Path $GameDir "dxgi.dll.acvbak"

function Hash([string]$path) { (Get-FileHash $path -Algorithm SHA256).Hash }

# ---------- uninstall ----------
if ($Uninstall) {
    if (-not (Test-Path $manifest)) { Write-Host "No acv_fsr4_install.txt in the game folder: the mod is not installed."; return }
    foreach ($line in Get-Content $manifest) {
        if ($line -notmatch '^file=(.+?)\|sha256=([0-9A-F]+)$') { continue }
        $path = Join-Path $GameDir $Matches[1]
        if (-not (Test-Path $path)) { continue }
        if ((Hash $path) -eq $Matches[2]) { Remove-Item $path -Force; Write-Host "Removed $($Matches[1])." }
        else { Write-Warning "$($Matches[1]) changed since install; left untouched." }
    }
    if ((Test-Path $dxgiParked) -and -not (Test-Path $gameDxgi)) { Rename-Item $dxgiParked "dxgi.dll"; Write-Host "Restored dxgi.dll." }
    Remove-Item $manifest -Force
    Write-Host "Uninstalled."
    return
}

# ---------- build ----------
if (-not $NoBuild) {
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        $vsCmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $vsCmake) { $cmake = $vsCmake }
    }
    if (-not $cmake) { throw "cmake not found. Install it or use a 'Developer PowerShell for VS 2022'." }
    Write-Host "`nBuilding the release DLL..."
    & $cmake -S $root -B $buildDir -G "Visual Studio 17 2022" -A x64 -DACV_BUILD_FSR4_RELEASE=ON | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
    & $cmake --build $buildDir --config Release --target acv_fsr4
    if ($LASTEXITCODE -ne 0) { throw "Build failed (see output above)." }
}
if (-not (Test-Path $builtDll)) { throw "Missing $builtDll (build first, without -NoBuild)." }
foreach ($name in $ffxDlls) { if (-not (Test-Path (Join-Path $ffxBinDir $name))) { throw "Missing SDK DLL $name." } }

# ---------- install ----------
$sources = [ordered]@{ "d3d12.dll" = $builtDll }
foreach ($name in $ffxDlls) { $sources[$name] = Join-Path $ffxBinDir $name }

# Refuse to overwrite files that are not ours (a previous install of this mod may be updated in place).
$previous = @{}
if (Test-Path $manifest) {
    foreach ($line in Get-Content $manifest) { if ($line -match '^file=(.+?)\|sha256=([0-9A-F]+)$') { $previous[$Matches[1]] = $Matches[2] } }
}
foreach ($name in $sources.Keys) {
    $dst = Join-Path $GameDir $name
    if (-not (Test-Path $dst)) { continue }
    $h = Hash $dst
    $isOurs = ($previous[$name] -eq $h) -or ($h -eq (Hash $sources[$name]))
    if (-not $isOurs) { throw "$name already exists in the game folder and is not from this mod; not overwriting it." }
}
if (Test-Path (Join-Path $GameDir "acvfsr_ffx_test.txt")) { Remove-Item (Join-Path $GameDir "acvfsr_ffx_test.txt") -Force }

# The old dxgi.dll proxy from the investigation is parked (not needed; avoids two proxies).
if ((Test-Path $gameDxgi) -and -not (Test-Path $dxgiParked)) {
    Rename-Item $gameDxgi "dxgi.dll.acvbak"
    Write-Host "Parked dxgi.dll as dxgi.dll.acvbak (restored on uninstall)."
}

$lines = @("# ACV FSR 4 mod install manifest (used by install_fsr4.ps1 -Uninstall)", "installed=$(Get-Date -Format s)")
foreach ($name in $sources.Keys) {
    Copy-Item $sources[$name] (Join-Path $GameDir $name) -Force
    $lines += "file=$name|sha256=$(Hash (Join-Path $GameDir $name))"
    Write-Host "Installed $name."
}
$lines | Set-Content $manifest

Write-Host "`nDone. Start Valhalla from Steam."
Write-Host "  Valhalla FSR setting ON (Quality etc.) -> FSR 4 upscaling.  FSR OFF -> FSR 4 native AA."
Write-Host "  F2 = settings menu (arrows + Enter): FSR 4, sharpness, jitter phases (8/16/32), skip stock AA, indicator."
Write-Host "  F7 = FSR 4 on/off, F4 = skip the game's own TAA/FSR 1 work on/off, F8/F9 = sharpness -/+, F3 = indicator always on/off."
Write-Host "  Each key shows an on-screen message; sharpness/F4/F3 are remembered between sessions."
Write-Host "  Top-left square for 3 s after start / F7 / mode change: green = FSR 4 upscaling, cyan = FSR 4 native. None = stock."
Write-Host "  Log: $env:LOCALAPPDATA\ACVFSR4\acv_fsr4.log"
Write-Host "  Remove with: powershell -ExecutionPolicy Bypass -File .\install_fsr4.ps1 -Uninstall"
