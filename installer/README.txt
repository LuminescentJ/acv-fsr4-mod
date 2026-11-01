FSR 4 for Assassin's Creed Valhalla (unofficial fan mod)  -  version 1.0.0

Replaces the game's FSR 1 upscaler with AMD FSR 4.

REQUIREMENTS
- Assassin's Creed Valhalla on PC. Developed and tested on the Steam version only.
- An AMD Radeon RX 6000 series graphics card or newer. Tested on an RX 9070 XT only. RX 7000 cards are supported by the
  included FSR files but untested here. RX 6000 cards are untested and may get FSR 3.1 instead of FSR 4.
  The FSR_CONTEXT line in the log shows which version is running.
- Offline single-player only. Do not use it in any online mode.

INSTALL
1. Close the game.
2. Run ACV_FSR4_Setup.exe.
3. Check the game folder (it is found automatically on most systems; use Browse if not) and click Install.
   Windows may ask for permission if the game folder is protected.

Windows SmartScreen or your antivirus may warn about the file because it is not code-signed by a paid certificate.
Check the VirusTotal result on the mod page if you want to be sure.

UNINSTALL
Run ACV_FSR4_Setup.exe again and click Uninstall. Only the mod's own files are removed; if another mod replaced one of
them, it is left in place. Your settings stay in %LOCALAPPDATA%\ACVFSR4 (a few small text files, safe to delete).

USE
- FSR ON in Valhalla's graphics settings: FSR 4 upscaling. Quality is the tested preset; the others should work.
- FSR OFF: FSR 4 as anti-aliasing at full resolution.
- A square at the top left for a few seconds after start-up shows that FSR 4 is active: green = upscaling, cyan = anti-aliasing.

KEYS
F2  settings menu (Up/Down choose, Left/Right or Enter change): FSR 4, sharpness, jitter phases, skip stock AA, indicator
F7  FSR 4 on/off (off = the stock game, handy for comparing)
F4  skip the game's own TAA and FSR 1 work on/off (on = a little faster)
F8 / F9  sharpness down / up
F3  indicator square always on/off

TROUBLESHOOTING
- No square and F7 does nothing: the mod did not load. Check that no other mod uses d3d12.dll.
- A log is written to %LOCALAPPDATA%\ACVFSR4\acv_fsr4.log. Include it when you report a problem.
- A game update can stop the mod from working. It should then leave the game running as stock, but that is not guaranteed.

NOTES
This mod is not affiliated with or endorsed by AMD or Ubisoft. Use at your own risk.
AMD's FSR files are included under the licence in AMD_LICENSE.txt.
