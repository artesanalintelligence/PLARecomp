# Builds nvnvk (tracer and backend) with MSVC + Ninja into build\port\nvnvk.
#   .\tools\port\nvnvk\build.ps1 [-Config Release|Debug]
param([string]$Config = 'Release')
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
. (Join-Path $repo 'recomp\scripts\env.ps1')
$src = Join-Path $repo 'tools\port\nvnvk'
$bld = Join-Path $repo 'build\port\nvnvk'
& python (Join-Path $src 'gen_tables.py') | Out-Host
& "$env:SystemRoot\System32\cmd.exe" /c "`"$env:MK8R_VCVARS`" >nul 2>nul && cmake -S `"$src`" -B `"$bld`" -G Ninja -DCMAKE_BUILD_TYPE=$Config >nul && cmake --build `"$bld`" 2>&1"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
