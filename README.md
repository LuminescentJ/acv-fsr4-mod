# FSR 4 for Assassin's Creed Valhalla

Source code for an unofficial fan mod that replaces the game's FSR 1 upscaler with AMD FSR 4, using the AMD FSR SDK. It is for offline single-player only and is not affiliated with AMD or Ubisoft.

## How it works

The mod is a `d3d12.dll` proxy placed in the game folder. It forwards calls to the real Windows `d3d12.dll` and hooks Direct3D 12 to find the game's TAA and FSR 1 passes. It then takes the colour, depth, motion and jitter data from those passes and runs FSR 4 through AMD's FidelityFX API.

Things a scanner may object to, and why they are there:

- **API hooking:** the proxy hooks D3D12 device and command list methods to read the game's rendering data.
- **Memory patching:** only when the optional Jitter Phases setting is above 8, the mod changes one jump table entry in `ACValhalla.exe` while the game runs. See `applyJitterPhases` in `src/d3d12_probe.cpp`. It first checks that the code matches the expected bytes, and it does nothing on a different build.
- **Installer:** `installer/acv_fsr4_setup.cpp` is an unsigned exe with the mod's DLLs embedded as resources. It asks for admin rights only if the game folder is not writable.

The mod writes only to its settings and log folder, `%LOCALAPPDATA%\ACVFSR4`. It has no network code.

## Layout

- `src/` - the proxy. `d3d12_probe.cpp` builds both the mod and a development probe; `ACV_FSR4_RELEASE=1` selects the mod. The `dxgi` files are an earlier experiment and are not part of the mod.
- `installer/` - the installer and uninstaller, plus the player readme.
- `build_installer.ps1` - builds the mod and the installer.
- `install_fsr4.ps1` - developer install and uninstall script.

## Building

Needs Visual Studio 2022 Build Tools with the C++ workload and the Windows SDK.

1. Clone the [AMD FSR SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), tag `v2.3.0`, into `third_party/FidelityFX-SDK`. It is not included here.
2. Run:

```
powershell -ExecutionPolicy Bypass -File .\build_installer.ps1
```

The output goes to `dist/`.

The script's packaging step also expects a `README.txt` in `installer/`, which is included.
