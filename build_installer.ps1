<#
 Builds the player-facing installer and the zip for Nexus Mods.
   dist\ACV_FSR4_Setup.exe            one-file installer/uninstaller (installer\acv_fsr4_setup.cpp + the three mod DLLs)
   dist\ACV_FSR4_Mod_1.0.0.zip        the exe, README.txt and AMD_LICENSE.txt
 Usage: powershell -ExecutionPolicy Bypass -File .\build_installer.ps1
#>
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$version = "1.0.1"
$src = Join-Path $root "installer"
$work = Join-Path $root "build_installer"
$dist = Join-Path $root "dist"
$payload = Join-Path $work "payload"
$ffxBin = Join-Path $root "third_party\FidelityFX-SDK\Kits\FidelityFX\signedbin"
$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"

# Mod DLL, always rebuilt so the installer can't carry a stale one.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" }
$buildDir = Join-Path $root "build_release"
& $cmake -S $root -B $buildDir -G "Visual Studio 17 2022" -A x64 -DACV_BUILD_FSR4_RELEASE=ON | Out-Null
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
& $cmake --build $buildDir --config Release --target acv_fsr4 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Mod build failed." }

New-Item -ItemType Directory -Force $payload, $dist | Out-Null
Copy-Item (Join-Path $buildDir "release\bin\Release\d3d12.dll") $payload -Force
foreach ($name in "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll") { Copy-Item (Join-Path $ffxBin $name) $payload -Force }
foreach ($name in "acv_fsr4_setup.cpp", "acv_fsr4_setup.rc", "acv_fsr4_setup.manifest", "resource.h") { Copy-Item (Join-Path $src $name) $work -Force }

# Icon: an "F4" tile (32 and 256 px PNG entries).
Add-Type -AssemblyName System.Drawing
$images = foreach ($size in 32, 256) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = "AntiAlias"; $g.TextRenderingHint = "AntiAliasGridFit"
    $g.Clear([System.Drawing.Color]::FromArgb(150, 24, 28))
    $font = New-Object System.Drawing.Font "Segoe UI", ($size * 0.42), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $format = New-Object System.Drawing.StringFormat
    $format.Alignment = "Center"; $format.LineAlignment = "Center"
    $g.DrawString("F4", $font, [System.Drawing.Brushes]::White, (New-Object System.Drawing.RectangleF 0, 0, $size, $size), $format)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , @($size, $ms.ToArray())
}
$ico = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $ico
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($i in $images) {
    $dim = if ($i[0] -ge 256) { 0 } else { $i[0] }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$i[1].Length); $w.Write([uint32]$offset)
    $offset += $i[1].Length
}
foreach ($i in $images) { $w.Write([byte[]]$i[1]) }
[System.IO.File]::WriteAllBytes((Join-Path $work "acv_fsr4.ico"), $ico.ToArray())

# Compile. The exe is statically linked, so players need no Visual C++ runtime.
$exe = Join-Path $dist "ACV_FSR4_Setup.exe"
$cmd = "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && cd /d `"$work`" && rc /nologo acv_fsr4_setup.rc && " +
       "cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX acv_fsr4_setup.cpp acv_fsr4_setup.res " +
       "/Fe:`"$exe`" /link /SUBSYSTEM:WINDOWS /DEPENDENTLOADFLAG:0x800 /DYNAMICBASE /NXCOMPAT " +
       "user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib advapi32.lib bcrypt.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Installer build failed." }

# AMD's licence requires its notice to travel with the DLLs (signedbin is listed under the MIT terms in license.md).
$license = Get-Content (Join-Path $root "third_party\FidelityFX-SDK\Kits\FidelityFX\docs\license.md") -Encoding UTF8
$notice = @("amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll are part of the AMD FSR SDK 2.3.0:", "") + $license[887..893]
$package = Join-Path $work "package"
if (Test-Path $package) { Remove-Item $package -Recurse -Force }
New-Item -ItemType Directory $package | Out-Null
$notice | Set-Content (Join-Path $package "AMD_LICENSE.txt") -Encoding UTF8
Copy-Item $exe, (Join-Path $src "README.txt") $package
$zip = Join-Path $dist "ACV_FSR4_Mod_$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $package "*") -DestinationPath $zip
Get-Item $exe, $zip | ForEach-Object { "{0}  {1:N1} MB" -f $_.FullName, ($_.Length / 1MB) }
