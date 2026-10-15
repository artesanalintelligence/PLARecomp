# PLARecomp

Static recompilation and native PC port toolkit for a Nintendo Switch title, identified by its
title ID **`01001F5010DFA000`** (version 1.1.1).

PLARecomp turns *your own* legally dumped copy of the game into a native Windows x86-64 build:
the game's AArch64 code is recompiled ahead of time to C and then to x86-64 DLLs on your machine,
and our runtime replaces the console platform with native code (input, files, time, audio and
services) plus an NVN to Vulkan graphics backend. On top of that it adds unlocked framerate,
resolution scaling, an in-game options menu, keyboard/mouse support and mods.

> **No game content is included.** This repository and its releases contain no game code, no
> generated or recompiled code, no assets, no keys and no data derived from the game binary.
> You need your own dump of the game (base + update 1.1.1) and the keys of your own console.
> Pirated or leaked material is not supported. See the [legal notice](#legal-notice).

## Install (players)

1. Download **`PLARecompSetup.exe`** from the [GitHub Releases](../../releases) page.
2. Run it (no administrator rights needed). It walks you through five steps:
   - **Your game**: pick your dump (NSP/NSZ/XCI/XCZ, file or folder) and your `prod.keys` /
     `title.keys` (they are found automatically in the usual emulator folders).
     The setup checks that the dump is the supported version.
   - **Check**: disk space, RAM and the components that will be unpacked.
   - **Install**: extracts the executable from your dump, recompiles it with the bundled portable
     compiler (including the native "islands" for the hottest game functions, generated from your
     executable) and assembles a playable folder. This takes a while (tens of minutes) and needs
     several GB of temporary disk space.
   - **Done**: creates shortcuts. Options are inside the game (press Esc).
3. Requirements: Windows 10 (1803+) or 11, 64-bit, a Vulkan-capable GPU driver. Nothing else:
   Python and the compiler are bundled in the setup.

## Build from source (developers)

Prerequisites: Windows 10/11 x64, Visual Studio 2022 Build Tools (C++ and Windows SDK), LLVM
(clang-cl), CMake, Ninja, Python 3.10+, Git.

```
git clone --recursive https://github.com/artesanalintelligence/PLARecomp
```

| Component | Where | How to build |
|---|---|---|
| Recompiler / runtime (fork of a GPL-3.0 Switch emulator with an AArch64 to C AOT recompiler) | external fork; its full source ships with each release | [recomp/README.md](recomp/README.md) |
| Native platform layer (`nnshim`) | `tools/port/nnshim` | `tools/port/nnshim/CMakeLists.txt` |
| NVN to Vulkan backend (`nvnvk`) and shader translation | `tools/port/nvnvk`, `tools/port/nvnshader` | `CMakeLists.txt` in each folder |
| Recompiled-code mods | `recomp/mods` | [recomp/MODDING.md](recomp/MODDING.md) |
| Native islands (hot functions lifted to C++ from your `main.elf` at install time) | `tools/islands`, `recomp/mods/islands` | `tools/islands/build_islands.py` |
| Portable compiler package | `tools/toolchain` | `tools/toolchain/build-toolchain.ps1` |
| Install pipeline (launcher) | `tools/launcher` | `tools/launcher/launcher.py --help` |
| Setup program and release | `tools/installer` | `tools/installer/build.ps1`, `tools/installer/make_release.py` |
| Extraction from your dump | `tools/extract` | [tools/extract/README.md](tools/extract/README.md) |
| Analysis (Ghidra, triage, ELF analysis) | `tools/ghidra`, `tools/triage`, `tools/analysis` | [tools/ghidra/README.md](tools/ghidra/README.md) |
| Switch toolchain (for the decomp harness) | `toolchain/` | [toolchain/README.md](toolchain/README.md) |

All generated material (extracted executables, analysis outputs, generated C, DLLs, logs) is
written outside the repository or into gitignored folders.

## Documentation

- Recompiler and runtime: [recomp/README.md](recomp/README.md)
- Writing mods: [recomp/MODDING.md](recomp/MODDING.md)
- Extraction tools: [tools/extract/README.md](tools/extract/README.md)
- Analysis with Ghidra: [tools/ghidra/README.md](tools/ghidra/README.md)
- Switch toolchain: [toolchain/README.md](toolchain/README.md)

## Licenses

- This repository's own code and docs: GNU GPL v3.0 or later, see [LICENSE](LICENSE).
- The recompiler/runtime fork and its emulator base are GPL-3.0-or-later; the release bundles
  third-party components (Qt under LGPL, SDL2 under Zlib, Dear ImGui under MIT, the Lato font
  under OFL, LLVM under Apache-2.0 with LLVM exception, Python under PSF-2.0) with their license
  texts.
- Every release that ships the exporter binary also ships its complete corresponding source
  (`plarecomp-exporter-source-<version>.tar.xz`, on the same release page), as the GPL requires.
- Submodules `toolchain/musl` and `tools/common` keep their own licenses.

## Legal notice

- This project is not affiliated with or endorsed by the game's publisher, developer or the
  console manufacturer. The game, its code and its assets belong to their respective owners.
- It contains no game code, assets, keys or firmware. Patches and mods refer to the game only
  by offsets and by the few instruction words they check or replace, as usual for game patches.
  Everything else is generated on the user's machine from the user's own copy.
- Keys must come from the user's own console (for example with Lockpick_RCM). The tools never
  download, bundle or share keys, and they are never copied into the output.
- The project exists for interoperability: running a legitimately owned copy of the game on
  hardware the user owns. Do not use it with pirated or leaked material.
