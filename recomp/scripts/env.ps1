# Common environment for the recomp/ scripts (dot-source: . .\recomp\scripts\env.ps1)
#
# Reason: on this machine the first cmake.exe on PATH is devkitPro/MSYS2's
# (c:\devkitPro\msys2\usr\bin), which does not work with MSVC. vcvars64.bat appends
# Visual Studio's CMake at the END of PATH, so it does not fix it. Here Kitware's
# native CMake is put first and VS 2022's vcvars64.bat is pinned.

$global:PLA_RECOMP_ROOT = if ($env:PLA_RECOMP_ROOT) { $env:PLA_RECOMP_ROOT } else { 'D:\PLARecomp' }
$global:MK8R_ROOT_DIR   = Join-Path $PLA_RECOMP_ROOT 'mk8-recomp'

$env:MK8R_ROOT = $MK8R_ROOT_DIR
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
# Without VS installed (portable toolchain, tools/toolchain) there is no vswhere: not an error.
if (-not $env:MK8R_VCVARS -and (Test-Path -LiteralPath $vswhere)) {
    $vs = & $vswhere -version '[17.0,18.0)' -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1
    if ($vs) { $env:MK8R_VCVARS = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' }
}
# The suyu exporter (game_export.cpp) looks for vcvars on its own; SUYU_VCVARS pins it.
if ($env:MK8R_VCVARS -and -not $env:SUYU_VCVARS) { $env:SUYU_VCVARS = $env:MK8R_VCVARS }

$kitwareCMake = 'C:\Program Files\CMake\bin'
$llvm = 'C:\Program Files\LLVM\bin'
foreach ($p in @($llvm, $kitwareCMake)) {
    if ((Test-Path $p) -and -not ($env:Path -split ';' | Select-Object -First 1).Equals($p)) {
        $env:Path = "$p;$env:Path"
    }
}

# The user's own dump (the repo's dump/ folder). The NSPs are found by the title id in their file name.
$global:PLA_DUMP_DIR  = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\dump'))
function Find-PlaDumpNsp([string]$TitleId) {
    if (-not (Test-Path -LiteralPath $PLA_DUMP_DIR)) { return $null }
    $f = Get-ChildItem -LiteralPath $PLA_DUMP_DIR -Filter '*.nsp' -File |
        Where-Object { $_.Name.Contains("[$TitleId]") } | Select-Object -First 1
    if ($f) { $f.FullName } else { $null }
}
$global:PLA_BASE_NSP   = Find-PlaDumpNsp '01001F5010DFA000'
$global:PLA_UPDATE_NSP = Find-PlaDumpNsp '01001F5010DFA800'
$global:PLA_TITLE_ID   = '01001F5010DFA000'

# In case windeployqt did not copy some Qt DLL (Charts, Concurrent) next to suyu.exe.
$qtBin = Join-Path $PLA_RECOMP_ROOT 'Qt\6.9.3\msvc2022_64\bin'
if ((Test-Path $qtBin) -and -not (($env:Path -split ';') -contains $qtBin)) { $env:Path = "$env:Path;$qtBin" }
