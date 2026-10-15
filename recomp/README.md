# Static recompilation of PLA 1.1.1 with suyu (mk8-recomp fork)

This folder holds our own documentation and scripts for bringing the game's v1.1.1
executable (Switch) to native x86-64 on Windows. We use the AArch64 → C → x86-64 AOT
recompiler of dougchansan's suyu fork (GPL-3.0-or-later):

- <https://github.com/dougchansan/mk8-recomp>: scripts, tests and notes.
- <https://github.com/dougchansan/suyu-v0.0.4>, branch `mk8-recomp`: the emulator and the
  recompiler. It is a submodule of mk8-recomp (`third_party/suyu`). In this session's clone
  it is at `v0.0.11-1-g5949cab3ba` (ABI 6).

Everything heavy (clones, build, exports, generated C, DLLs and logs) lives in `D:\PLARecomp\`,
outside the repo. The repo only holds our own code and documentation. No binary, key,
generated code or game data is ever committed.

The current status, figures and next steps are in [STATUS.md](STATUS.md). How to write and
enable native mods (function overrides without recompiling the game) is in
[MODDING.md](MODDING.md).

Our own changes to the fork (translation of SHL and the other shifts by immediate, mod
runtime) are in the local `pla-mods` branch of `D:\PLARecomp\mk8-recomp` and of its
`third_party\suyu` submodule (unpublished).

## Layout

```
recomp/
  README.md, STATUS.md
  scripts/
    env.ps1                   common environment (VS 2022 vcvars, Kitware CMake first, paths)
    stage-suyu-userdata.py    prod.keys + title.keys (from the NSP tickets) + update into the NAND
    build-suyu-pla.ps1        builds suyu + suyu-cmd (wraps scripts\build-suyu.ps1)
    export-pla.ps1            click-free AOT export through suyu's MCP server
    make-roots.py             all_code_targets.json -> <module>.roots (SUYU_AOT_EXTRA_ROOTS)
    build-modules.ps1         compiles the generated modules to DLLs with clang-cl
    run-pla.ps1               starts PLA (JIT or Hybrid) unattended and saves logs/gaps
    list-suyu-dialogs.ps1     lists suyu dialogs (UI Automation), to debug hangs
    click-suyu-button.ps1     presses a button of a suyu dialog (e.g. "Firmware Not Found")
    setup-portable-suyu.ps1   copy of suyu with its own user directory (D:\PLARecomp\suyu-portable)
    run-pla-portable.ps1      starts PLA in that copy (own MCP port, screenshots, does not kill other suyu)
    build-mod.ps1             compiles a native mod with clang-cl and installs it
  mods/
    include/pla_mod_api.h     C API for mods (copy of the fork's)
    hid_input_logger/         example mod (nn::hid::GetNpadState / GetNpadStyleSet)
  MODDING.md                  mod guide
    run-pla-safe.py           starts PLA without closing other suyu.exe (only handles the process it creates)
    validate-fps-patch.ps1    tests a framerate mod (with or without limiter) and restores config and mods
  patches/
    unlocked-fps/             unlocked FPS with dynamic delta (JSON, pchtxt, IPS32); see docs/research/06-framerate.md
    60fps/                    fixed 60 FPS with corrected delta (by-product)

D:\PLARecomp\
  mk8-recomp\                 clone with submodules (third_party\suyu = the fork)
    build\suyu\bin\           suyu.exe, suyu-cmd.exe
    build\recomp\pla\<mod>\   recompiled_*.dll (loaded by run-hybrid.ps1 through SUYU_RECOMP_DIR)
    local\tools\Qt            junction -> D:\PLARecomp\Qt
  Qt\6.9.3\msvc2022_64\       official Qt (aqtinstall)
  exports\pla-<tag>\pkg\      exported packages (aot_cache\exefs\<mod>\src\*.c)
  roots\pla-1.1.1-all\        main.roots generated from the static analysis
  logs\                       build, export and run logs, and recomp_gaps.json
```

## 1. Requirements and installation (one time)

What the machine already had: Windows 11, Git, Python 3.12, Kitware CMake 4.2.1
(`C:\Program Files\CMake\bin`), LLVM 23.1.2 with clang-cl (`C:\Program Files\LLVM\bin`),
the Vulkan SDK and a VS 2019 Build Tools not used here.

1. **Visual Studio 2022 Build Tools** with the C++ workload (MSVC 14.44 and Windows SDK
   10.0.26100). It needs an elevated console (UAC):

   ```powershell
   Invoke-WebRequest https://aka.ms/vs/17/release/vs_BuildTools.exe -OutFile D:\PLARecomp\vs_BuildTools.exe
   Start-Process D:\PLARecomp\vs_BuildTools.exe -Verb RunAs -Wait -ArgumentList `
     '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
   ```

   If the installer ends with code 5002 and `%TEMP%\dd_bootstrapper_*.log` shows an
   "access denied" error on `C:\Program Files (x86)\Microsoft Visual Studio\Installer`,
   another process is restricting writes to Program Files (for example a sandbox; it
   does not happen from a normal console). `winget install
   Microsoft.VisualStudio.2022.BuildTools` fails for the same reason.

2. **Qt 6.9.3 msvc2022_64** with aqtinstall, outside the repo, and linked where the
   mk8-recomp scripts expect it:

   ```powershell
   python -m pip install --user aqtinstall
   python -m aqt install-qt windows desktop 6.9.3 win64_msvc2022_64 -m qtcharts -O D:\PLARecomp\Qt --timeout 120
   ```

   If a download fails with `Read timed out`, run it again with `--timeout 120`.

3. **Source code** (about 2 GB with the submodules):

   ```powershell
   git clone https://github.com/dougchansan/mk8-recomp D:\PLARecomp\mk8-recomp
   git -C D:\PLARecomp\mk8-recomp submodule update --init --recursive --jobs 8
   New-Item -ItemType Junction -Path D:\PLARecomp\mk8-recomp\local\tools\Qt -Target D:\PLARecomp\Qt
   & D:\PLARecomp\mk8-recomp\scripts\bootstrap.ps1 -Root D:\PLARecomp\mk8-recomp -SkipGame   # downloads glslang 16.5.0
   ```

   `setup-windows.ps1` (`irm | iex`) is not needed. It was read: it only installs Git,
   CMake, Ninja, Python and VS Build Tools with winget, clones, and calls `bootstrap.ps1` and
   `build-suyu.ps1`. Here those steps were done by hand.

4. **Keys and update in suyu's virtual NAND** (`%APPDATA%\suyu`):

   ```powershell
   $d = '<repo>\dump'
   python recomp\scripts\stage-suyu-userdata.py `
     --nsp "$d\<base game> [01001F5010DFA000][v0].nsp" `
     --nsp "$d\<update> [01001F5010DFA800][v262144].nsp" `
     --install-update "$d\<update> [01001F5010DFA800][v262144].nsp"
   ```

   The script copies `~\.switch\prod.keys`, adds the update's common-ticket key to
   `title.keys` (without printing it) and copies the 5 update NCAs (about 93 MB) to
   `nand\user\Contents\registered`, which is the same thing "Install Files to NAND" does. The
   base NSP is **not** copied: both the exporter and the boot read it from its path and
   `PatchManager` applies the installed update on top (the log shows
   `ExeFS: Update (v0.4.0) applied successfully` and the title shows as 1.1.1).

   There is no firmware installed in suyu. Every boot shows the "Firmware Not Found" modal
   and `run-pla.ps1` accepts it by itself. The game boots anyway (HLE). To install
   firmware: Tools > Install Firmware.

## 2. Build suyu (fork) with the recompiler

```powershell
.\recomp\scripts\build-suyu-pla.ps1 -Configure     # suyu.exe + suyu-cmd.exe, Release, with JIT
.\recomp\scripts\build-suyu-pla.ps1 -NoJit         # variant without Dynarmic (for strict AOT)
```

Notes:
- `env.ps1` puts Kitware's CMake first in the PATH. On this machine the first `cmake.exe`
  in the PATH is the devkitPro/MSYS2 one, which does not work with MSVC, and `vcvars64.bat`
  appends VS's CMake at the **end** of the PATH. It also sets `MK8R_VCVARS`/`SUYU_VCVARS`
  to the VS 2022 vcvars.
- The host is compiled with MSVC (cl 19.44). The first configure downloads the dependencies
  with CPM (Boost, OpenSSL, etc.). The full build (1771 steps) takes about 4 minutes on the i9-14900KF.
- Do not redirect the output of `build-suyu.ps1` with `*>&1` inside PowerShell 5.1:
  every cmake stderr line becomes an ErrorRecord and `$ErrorActionPreference='Stop'`
  aborts. That is why `build-suyu-pla.ps1` launches it in a separate process with the
  redirection done at process level.

## 3. Test in JIT mode (unattended)

```powershell
.\recomp\scripts\run-pla.ps1 -Mode jit -Seconds 90
```

Launches `suyu.exe -hacker`, waits for the MCP server (127.0.0.1:9742), requests
`launch_game_path` with the base NSP, accepts the firmware modal, records
`fps`/`first_frame` every 10 s and stops emulation with `stop_emulation`. The logs
go to `D:\PLARecomp\logs\run-jit-*.log` and `*.suyu_log.txt`.

In the GUI: open `D:\PLARecomp\mk8-recomp\build\suyu\bin\suyu.exe`, File > Load File >
the base NSP, and accept "Firmware Not Found".

## 4. Export (Hybrid JIT + AOT)

```powershell
# Extra roots from the static analysis (tools\ghidra\out\all_code_targets.json)
python recomp\scripts\make-roots.py --json tools\ghidra\out\all_code_targets.json `
  --out D:\PLARecomp\roots\pla-1.1.1-all --text-size 0x32a5690

.\recomp\scripts\export-pla.ps1 -Backend hybrid -Format source -DropRomfs                 # without roots
.\recomp\scripts\export-pla.ps1 -Backend hybrid -Format source -DropRomfs `
  -Roots D:\PLARecomp\roots\pla-1.1.1-all                                                 # with roots
```

The exporter is a Qt dialog with no CLI. `export-pla.ps1` drives it through suyu's built-in
MCP server:
1. `trigger_ui_action {action: export_game}` opens `GameExportDialog`. The call does not
   return because the handler stays blocked in the modal's `exec()`, which is expected.
2. `trigger_ui_action {action: aot_test_export, rom_path, output_dir, format: source|build,
   backend: hybrid|static|dynarmic}` calls `TriggerExportForTesting`. It is a real export
   that skips the firmware and update dialogs.
3. Polls `get_aot_export_status` until `done`, closes the dialog and suyu cleanly
   (if the process is killed, the asynchronous logger loses the last lines) and saves the log and
   the `AOT coverage` lines to `D:\PLARecomp\logs\export-<tag>.*`.

The package ends up in `D:\PLARecomp\exports\pla-<tag>\pkg\` (the script renames the folder
named after the game title plus " - Hybrid AOT + JIT" to `pkg`, because the accented
character in the title breaks ASCII `.cmd` files). It contains `aot_cache\exefs\<module>\`
(CMake project + `src\*.c`), `aot_cache\aot_manifest.json` and `exefs\` with the NSOs and
`romfs.bin` (6 GB, which `-DropRomfs` deletes).

Extra roots: `game_export.cpp` reads `%SUYU_AOT_EXTRA_ROOTS%\<module>.roots` (one hexadecimal
offset per line, relative to `.text`, which starts at 0 in PLA's main) and adds them
to the `DiscoverBlocks` roots. The addresses recorded by Hybrid runs
(`recomp_gaps.json`) come in another way: the store `%APPDATA%\suyu\recomp\gaps\<TITLEID>.json`,
which the next export uses without any configuration.

`-Format build` also does what the GUI calls "Build": it compiles the modules with clang-cl
inside suyu's build tree and links a `suyu-cmd-static` launcher.

## 5. Compile the modules to DLLs and run in Hybrid

```powershell
.\recomp\scripts\build-modules.ps1 -Export D:\PLARecomp\exports\pla-hybrid-source-roots -Target pla
.\recomp\scripts\run-pla.ps1 -Mode hybrid -RecompDir D:\PLARecomp\mk8-recomp\build\recomp\pla -Seconds 300
```

`build-modules.ps1` configures each module with Ninja and `-DCMAKE_C_COMPILER=clang-cl`
(`/O2 /bigobj /fp:precise`), compiles `recompiled_image` (main) or `recompiled_<mod>` and
leaves the DLL in `mk8-recomp\build\recomp\pla\<mod>\`. The generated CMake limits
concurrency to RAM/8 GB (3 compilations at once with 32 GB). It can be changed with
`-Jobs N`, keeping an eye on memory.

`run-pla.ps1 -Mode hybrid` sets `SUYU_RECOMP_DIR` (suyu loads each `recompiled_*.dll` and
takes the module from the folder name), starts the game and on exit copies
`%APPDATA%\suyu\log\recomp_gaps.json` to `D:\PLARecomp\logs\run-hybrid-*.recomp_gaps.json`.
suyu dumps the gaps every 30 s and when the session ends.

## 6. Port v0: playable folder without suyu.exe

Result in `D:\PLARecomp\dist\PLA-PC-v0\` (instructions to play in its `README.txt`, whose
source is `recomp/dist/README.txt`). To regenerate it from your own dump:

```powershell
.\recomp\scripts\setup-portable-suyu.ps1                       # suyu isolated from %APPDATA%\suyu
python tools\patch\apply_patches.py -p recomp\patches\unlocked-fps\pla-111-unlocked-fps.json `
    --in data\exefs\main --out "D:\PLARecomp\suyu-portable\user\load\01001F5010DFA000\Unlocked FPS baked\exefs\main"
.\recomp\scripts\export-pla.ps1 -Backend hybrid -Format source -ShlImm -Tag v0 `
    -Roots D:\PLARecomp\roots\pla-1.1.1-all -SuyuExe D:\PLARecomp\suyu-portable\suyu.exe -McpPort 9772
python recomp\scripts\check-baked-patches.py -p recomp\patches\unlocked-fps\pla-111-unlocked-fps.json `
    --src D:\PLARecomp\exports\pla-v0\pkg\aot_cache\exefs\main\src          # 43/43
.\recomp\scripts\build-modules.ps1 -Export D:\PLARecomp\exports\pla-v0 -Target pla-v0 -Jobs 3   # ~45 min
.\recomp\scripts\make-dist.ps1 -Recomp D:\PLARecomp\mk8-recomp\build\recomp\pla-v0 -Export D:\PLARecomp\exports\pla-v0\pkg
```

Notes:
- **No `-DropRomfs`**: the export's `romfs.bin` goes into the dist (hard link) and is also used by
  `extract-romfs.py` (RomFS to a folder for the nnshim).
- The portable suyu needs the `nand/sdmc/load/dump` paths in its `qt-config.ini` pointing to
  its `user\` (the copied config had the `%APPDATA%` ones): `setup-portable-suyu.ps1` fixes them.
- If an emitter change only affects a few units, you can re-export, compare the
  `src\*.c` and copy only the changed ones over the already compiled export: Ninja recompiles only those
  units (2.4 min instead of 45 for the `LDR (literal)` fix).
- `send-key.ps1` presses keys in the launcher window (unattended input test).

New scripts: `make-dist.ps1`, `check-baked-patches.py`, `extract-romfs.py`, `send-key.ps1`.

## 7. nnshim (port v1-boot) as a mod

```powershell
cmd /c "`"$env:MK8R_VCVARS`" && cmake -S tools\port\nnshim -B D:\PLARecomp\build\nnshim -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build D:\PLARecomp\build\nnshim"
python recomp\scripts\extract-romfs.py D:\PLARecomp\exports\pla-v0\pkg\exefs\romfs.bin D:\PLARecomp\romfs
python tools\port\nnshim\gen_mod_json.py --out D:\PLARecomp\build\nnshim\all\mod.json `
    --romfs-dir D:/PLARecomp/romfs --save-root D:/PLARecomp/nnshim-save --input keyboard   # --only hid: input only
# install nnshim.dll + mod.json into <suyu user>\recomp\mods\01001F5010DFA000\nnshim\
```

## Licenses and rules

- suyu/mk8-recomp: GPL-3.0-or-later. The scripts in this folder are ours.
- The generated C is a translation of the game's code: it is not published, not committed and does not
  leave `D:\PLARecomp`.
- No leaked material from the game's developer is used.
