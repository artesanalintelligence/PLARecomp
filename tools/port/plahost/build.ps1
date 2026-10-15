# Builds plahost (PLA.exe without an emulator) with MSVC + Ninja into build\port\plahost.
#   .\tools\port\plahost\build.ps1 [-Config Release|Debug] [-Jobs 8]
param([string]$Config = 'Release', [int]$Jobs = 8)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
. (Join-Path $repo 'recomp\scripts\env.ps1')
$src = Join-Path $repo 'tools\port\plahost'
$bld = Join-Path $repo 'build\port\plahost'
$env:VSLANG = '1033'  # English compiler messages: ninja tracks header dependencies
& "$env:SystemRoot\System32\cmd.exe" /c "`"$env:MK8R_VCVARS`" >nul 2>nul && cmake -S `"$src`" -B `"$bld`" -G Ninja -DCMAKE_BUILD_TYPE=$Config >nul && cmake --build `"$bld`" -- -j $Jobs 2>&1"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Get-Item (Join-Path $bld 'PLA.exe') | Select-Object FullName, Length, LastWriteTime
