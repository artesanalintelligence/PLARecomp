PLARecomp - PLA 1.1.1 recompiled for Windows (local use only)
==============================================================

WHAT IT IS
  The game code (ARM64) translated to C and compiled to x86-64 (recompiled_*.dll), run by
  PLA.exe, a small native host (no emulator) that provides memory, threads and the few
  system services the game still asks for, and loads the native mods: nvnvk (own Vulkan
  renderer) and nnshim (files, saves, controls, audio). main has the unlocked FPS patch
  baked in (the game measures the real time of each frame).

  IMPORTANT: this folder contains data taken from YOUR copy of the game (exefs\, romfs.bin,
  recompiled_*.dll). It is for personal use on this computer only. Do not share it or
  upload it anywhere.

REQUIREMENTS
  - 64-bit Windows 10 (1803 or newer) / 11, GPU with Vulkan 1.3 (tested: RTX 4070 SUPER).
  - 16 GB of RAM minimum (the game gets 4.3 GB of guest memory and loads 1.4 GB of
    recompiled code). Close heavy programs before playing.
  - ~9 GB of disk space for this folder.
  - The Visual C++ runtime is included (msvcp140*.dll, vcruntime140*.dll).

STARTING
  Double-click PLA.exe. Options are in PLA.ini (created with defaults if missing):
    fullscreen=0      1 = borderless fullscreen (Alt+Enter toggles it while playing)
    nvnvk_fps_cap=0   FPS cap (e.g. 30 or 60; 0 = unlocked)
    mods=1            0 = only the required mods (nnshim and nvnvk)
    hang_report=30    if the game stops drawing for N seconds, writes user\log\threads-*.txt
  Command-line equivalents: --fps-cap=N, -f (fullscreen), --windowed, --no-mods.
  In game: Esc opens the PLARecomp menu (graphics, display, controls, sound); it saves
  fullscreen and the FPS cap into PLA.ini.

CONTROLS
  Gamepad: XInput/SDL is assigned to player 1 automatically when connected.
  Keyboard and mouse: see the Controls tab of the in-game menu (Esc).

WHERE EVERYTHING GOES
  PLA.exe, recompiled_*.dll        game host and recompiled code (do not touch)
  PLA.ini                          options (see above)
  exefs\                           game executables 1.1.1 (main with the patch) and
                                   romfs.bin (game data, 6 GB)
  fonts\                           free substitute system fonts
  user\save\                       save data
  user\log\pla.log                 log of the last run (pla.old.log: the one before;
                                   crash-*.txt / threads-*.txt reports)
  mods\01001F5010DFA000\<mod>\     native mods (mod.json + DLL, see recomp/MODDING.md).
                                   To disable an optional one, create a file named
                                   "disabled" in its folder.

HOW TO REBUILD IT
  Run the PLARecomp installer again (or tools/launcher/launcher.py from the repo) with
  your dump and your keys. Details in docs/port/LAUNCHER.md.
