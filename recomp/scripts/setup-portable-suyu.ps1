# Creates (or updates) a "portable" copy of suyu with its own user folder, isolated
# from the %APPDATA%\suyu used by other runs or the user:
#
#   <root>\suyu-portable\            copy of build\suyu\bin (without .map)
#   <root>\suyu-portable\user\       config, keys, nand, recomp\{gaps,mods}, log, load
#
# suyu uses <exe>\user\ instead of %APPDATA%\suyu when that folder exists
# (common/fs/path_util.cpp, PORTABLE_DIR). This way an export or a test run does not
# see the romfs/exefs mods installed in %APPDATA%\suyu\load (e.g. another run's
# 60 FPS patch, which would change the code bytes and send blocks to the JIT), does
# not share the log or the MCP port, and nobody kills this process by path name.
#
#   .\recomp\scripts\setup-portable-suyu.ps1            # first time: binaries + user data
#   .\recomp\scripts\setup-portable-suyu.ps1 -BinOnly   # after rebuilding suyu: binaries only

[CmdletBinding()]
param(
    [string]$Dest = '',
    [switch]$BinOnly
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
if (-not $Dest) { $Dest = Join-Path $PLA_RECOMP_ROOT 'suyu-portable' }
$bin = Join-Path $MK8R_ROOT_DIR 'build\suyu\bin'
if (-not (Test-Path -LiteralPath (Join-Path $bin 'suyu.exe'))) { throw "no suyu build in $bin" }
if (Get-Process suyu -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Dest*" }) {
    throw "the portable suyu in $Dest is running"
}

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
# robocopy: 0-7 mean success; /XF *.map leaves out the linker maps (125 MB).
& robocopy $bin $Dest /E /XF *.map /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed copying $bin ($LASTEXITCODE)" }
Write-Host "binaries copied to $Dest"

if (-not $BinOnly) {
    $src = Join-Path $env:APPDATA 'suyu'
    $user = Join-Path $Dest 'user'
    foreach ($d in 'config', 'keys', 'nand', 'sdmc', 'recomp\gaps') {
        $from = Join-Path $src $d
        if (Test-Path -LiteralPath $from) {
            & robocopy $from (Join-Path $user $d) /E /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -ge 8) { throw "robocopy failed copying $from ($LASTEXITCODE)" }
        }
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $user 'load'), (Join-Path $user 'log'),
        (Join-Path $user 'recomp\mods'), (Join-Path $user 'dump') | Out-Null
}

# The copied config stores absolute paths (nand/sdmc/load/dump) pointing to
# %APPDATA%\suyu: without this the "portable" copy would keep reading the mods and NAND there.
$cfg = Join-Path $Dest 'user\config\qt-config.ini'
if (Test-Path -LiteralPath $cfg) {
    $text = Get-Content -LiteralPath $cfg -Raw
    $u = (Join-Path $Dest 'user').Replace('\', '/')
    foreach ($k in 'nand', 'sdmc', 'load', 'dump') {
        $text = $text -replace "(?m)^${k}_directory=.*$", "${k}_directory=$u/$k"
    }
    [IO.File]::WriteAllText($cfg, $text, (New-Object Text.UTF8Encoding $false))
    Write-Host "nand/sdmc/load/dump paths of $cfg point to $u"
}
exit 0
