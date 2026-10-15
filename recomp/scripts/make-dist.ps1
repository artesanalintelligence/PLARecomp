# Assembles the port of PLA 1.1.1: a folder that is played by double-clicking PLA.exe.
#
#   <root>\dist\PLA-PC-v0\
#     PLA.exe (+ MSVC runtime)             plahost (tools/port/plahost, docs/port/HOST.md): runs the
#                                          recompiled code with no emulator; options in PLA.ini,
#                                          mods from mods\ next to it (nnshim and nvnvk required)
#     recompiled_*.dll                     recompiled game code (AOT), main with unlocked FPS
#     exefs\                               baked main, sdk, subsdk*, rtld, main.npdm and romfs.bin
#     mods\01001F5010DFA000\<id>\          native mods (mod.json + DLL) and romfs/exefs mods
#     user\                                save\ (nnshim's save data) and log\pla.log
#     README.txt
#
# No suyu-cmd.exe and no .bat launchers since 2026-10-09: PLA.exe needs no environment variables (the
# launcher writes PLA.ini). Leftovers of older assemblies are removed. Files are replaced, never written
# through: a re-assembled folder may hold hard links to another folder's DLLs.
#
# EVERYTHING in exefs\ and in the recompiled_*.dll files comes from the user's own dump: the folder is
# for local use only and is not distributed. No keys, NAND or emulator configuration go in: plahost
# needs none (the keys are only used by the installer to extract the dump).
#
#   .\recomp\scripts\make-dist.ps1 -Recomp <root>\mk8-recomp\build\recomp\pla-v0 `
#       -Export <root>\exports\pla-v0\pkg

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Recomp,
    [Parameter(Mandatory)][string]$Export,
    [string]$Dest = '',
    [string[]]$Mods = @('hid_input_logger'),
    # The game's executable (tools/port/plahost/build.ps1).
    [string]$Exe = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
if (-not $Dest) { $Dest = Join-Path $PLA_RECOMP_ROOT 'dist\PLA-PC-v0' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Exe) { $Exe = Join-Path $repo 'build\port\plahost\PLA.exe' }
if (-not (Test-Path -LiteralPath $Exe)) { throw "no plahost PLA.exe at $Exe (tools/port/plahost/build.ps1)" }
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# Copies a file by replacing the destination (never writing through a hard link to another folder).
function Put-File([string]$From, [string]$To) {
    if (Test-Path -LiteralPath $To) { Remove-Item -LiteralPath $To -Force }
    Copy-Item -LiteralPath $From -Destination $To
}

# 0. Leftovers of older assemblies (emulator frontends, the .bat launchers, emulator-named logs).
foreach ($old in 'suyu-cmd.exe', 'PLA-fork.exe', 'PLA.bat', 'PLA-unlimited.bat', 'PLA-strict.bat', 'PLAY.bat',
                 'user\config\sdl2-unlimited.ini', 'user\log\suyu_log.txt', 'user\log\suyu_log.txt.old.txt') {
    $f = Join-Path $Dest $old
    if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force }
}

# 1. The game's executable and the MSVC runtime the recompiled images import (redistributable).
Put-File $Exe (Join-Path $Dest 'PLA.exe')
foreach ($d in 'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
               'vcruntime140.dll', 'vcruntime140_1.dll') {
    $src = Join-Path $env:SystemRoot "System32\$d"
    if (Test-Path -LiteralPath $src) { Put-File $src (Join-Path $Dest $d) }
}

# 2. AOT images: PLA.exe looks for them next to the .exe with these names.
$names = @{ main = 'recompiled_image.dll'; rtld = 'recompiled_rtld.dll'; sdk = 'recompiled_sdk.dll'
            subsdk0 = 'recompiled_subsdk0.dll'; subsdk1 = 'recompiled_subsdk1.dll' }
foreach ($m in $names.Keys) {
    $dll = Get-ChildItem -LiteralPath (Join-Path $Recomp $m) -Filter $names[$m] -Recurse -File | Select-Object -First 1
    if (-not $dll) { throw "missing $($names[$m]) in $Recomp\$m" }
    Put-File $dll.FullName (Join-Path $Dest $names[$m])
}

# 3. Effective ExeFS (update + baked main) and the export's RomFS. romfs.bin (6 GB) as a
#    hard link if it is on the same volume.
$exefsOut = Join-Path $Dest 'exefs'
New-Item -ItemType Directory -Force -Path $exefsOut | Out-Null
foreach ($f in Get-ChildItem -LiteralPath (Join-Path $Export 'exefs') -File) {
    $to = Join-Path $exefsOut $f.Name
    if (Test-Path -LiteralPath $to) { Remove-Item -LiteralPath $to -Force }
    if ($f.Length -gt 1GB -and ([IO.Path]::GetPathRoot($f.FullName) -eq [IO.Path]::GetPathRoot($to))) {
        New-Item -ItemType HardLink -Path $to -Target $f.FullName | Out-Null
    } else {
        Copy-Item -LiteralPath $f.FullName -Destination $to
    }
}
if (-not (Test-Path (Join-Path $exefsOut 'romfs.bin'))) { Write-Warning 'the export has no romfs.bin (was it exported with -DropRomfs?)' }

# 4. User folder: save data (nnshim) and logs. Nothing of the emulator (config, keys, NAND).
$user = Join-Path $Dest 'user'
New-Item -ItemType Directory -Force -Path (Join-Path $user 'save'), (Join-Path $user 'log') | Out-Null

# 5. Native mods (e.g. -Mods hid_input_logger,render_res,engine_options).
# powershell -File passes "a,b,c" as one string: split it.
$Mods = @($Mods | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$modsOut = Join-Path $Dest "mods\$PLA_TITLE_ID"
New-Item -ItemType Directory -Force -Path $modsOut | Out-Null
foreach ($m in $Mods) {
    & (Join-Path $PSScriptRoot 'build-mod.ps1') -Mod (Join-Path $repo "recomp\mods\$m") -Install `
        -ModsRoot (Join-Path $Dest 'mods') | Out-Host
}

# 6. README. (No launchers: PLA.exe reads PLA.ini and finds mods\ by itself.)
Copy-Item (Join-Path $repo 'recomp\dist\README.txt') $Dest -Force
$size = (Get-ChildItem -LiteralPath $Dest -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("port in {0} ({1:n1} GB)" -f $Dest, ($size / 1GB))
exit 0
