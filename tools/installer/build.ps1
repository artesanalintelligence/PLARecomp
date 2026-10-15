# Builds the setup program (PLARecompSetup.exe): C++ + Dear ImGui (Win32 + Direct3D 11), one .exe with the static
# CRT and the Lato fonts as resources. This is the "bare" .exe (no components). tools\installer\make_release.py
# appends the components to make the release .exe. Needs VS 2022 Build Tools (cl, rc, Windows SDK) on the build PC.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\installer\build.ps1 [-Out D:\PLARecomp\installer] [-Debug] [-DevIni]
# -DevIni writes PLARecompSetup.ini with repo=<this checkout> next to the .exe (developer mode: no components).
param([string]$Out = 'D:\PLARecomp\installer', [switch]$Debug, [switch]$DevIni)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
$imgui = Join-Path $repo 'tools\port\nvnvk\third_party\imgui'
$lato = Join-Path $repo 'tools\port\nvnvk\third_party\fonts\lato'
$dx11 = Join-Path $here 'third_party\imgui_dx11'

$vcvars = $env:MK8R_VCVARS
if (-not $vcvars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found: install VS 2022 Build Tools (C++).' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio with the x64 C++ tools was found.' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
}

New-Item -ItemType Directory -Force $Out | Out-Null
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null

$ico = Join-Path $obj 'setup.ico'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'icon.ps1') -Out $ico
if ($LASTEXITCODE -ne 0) { throw 'Icon generation failed.' }

$f = { param($p) ($p -replace '\\', '/') }
$rc = @"
#include "resources.h"
kResIcon ICON "$(& $f $ico)"
kResLatoRegular RCDATA "$(& $f (Join-Path $lato 'Lato-Regular.ttf'))"
kResLatoBold RCDATA "$(& $f (Join-Path $lato 'Lato-Bold.ttf'))"
kResLatoBlack RCDATA "$(& $f (Join-Path $lato 'Lato-Black.ttf'))"
1 24 "$(& $f (Join-Path $here 'src\app.manifest'))"
"@
$rcPath = Join-Path $obj 'setup.rc'
[IO.File]::WriteAllText($rcPath, $rc, (New-Object Text.UTF8Encoding $false))

$exe = Join-Path $Out 'PLARecompSetup.exe'
$src = @('main.cpp', 'app.cpp', 'ui.cpp', 'engine.cpp', 'components.cpp') | ForEach-Object { Join-Path $here "src\$_" }
$src += @((Join-Path $imgui 'imgui.cpp'), (Join-Path $imgui 'imgui_draw.cpp'), (Join-Path $imgui 'imgui_tables.cpp'),
    (Join-Path $imgui 'imgui_widgets.cpp'), (Join-Path $imgui 'backends\imgui_impl_win32.cpp'), (Join-Path $dx11 'imgui_impl_dx11.cpp'))
$opt = if ($Debug) { '/Od /Zi /MTd' } else { '/O2 /Gy /Gw /MT /DNDEBUG' }
$cl = "cl /nologo /std:c++20 /EHsc /utf-8 /W3 /MP $opt /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS " +
      "/DIMGUI_DISABLE_DEMO_WINDOWS /DIMGUI_DISABLE_DEBUG_TOOLS " +
      "/I`"$here\src`" /I`"$imgui`" /I`"$imgui\backends`" /I`"$dx11`" /Fo`"$obj\\`" /Fe`"$exe`" " +
      (($src | ForEach-Object { "`"$_`"" }) -join ' ') + " `"$obj\setup.res`" " +
      "/link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib windowscodecs.lib " +
      "ole32.lib shell32.lib user32.lib gdi32.lib uuid.lib imm32.lib bcrypt.lib winhttp.lib"
$bat = Join-Path $obj 'build.cmd'
$txt = "@echo off`r`ncall `"$vcvars`" >nul || exit /b 1`r`nrc /nologo /I`"$here\src`" /fo`"$obj\setup.res`" `"$rcPath`" || exit /b 1`r`n$cl || exit /b 1`r`n"
[IO.File]::WriteAllText($bat, $txt, (New-Object Text.UTF8Encoding $false))
& "$env:SystemRoot\System32\cmd.exe" /d /c "`"$bat`""
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }

if ($DevIni) {
    $ini = Join-Path $Out 'PLARecompSetup.ini'
    if (-not (Test-Path $ini)) {
        $t = "; Developer mode: scripts from this checkout, system Python, no bundled components.`r`n[Setup]`r`nrepo=$repo`r`n"
        [IO.File]::WriteAllText($ini, $t, (New-Object Text.UnicodeEncoding $false, $true))
    }
}
$kb = [math]::Round((Get-Item $exe).Length / 1KB)
Write-Host "Done: $exe ($kb KB)"
