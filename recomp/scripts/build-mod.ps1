# Builds a native mod (recomp\mods\<mod>\*.c + mod.json) into a DLL with clang-cl and,
# with -Install, copies it to the suyu mods folder:
#   %APPDATA%\suyu\recomp\mods\<TITLEID>\<id>\{mod.json, <library>.dll}
# The runtime (core\arm\recomp\recomp_mods.cpp in the fork) loads them when the game
# starts with the recompiled DLLs; main does not need to be re-exported or recompiled.
#
#   .\recomp\scripts\build-mod.ps1 -Mod recomp\mods\hid_input_logger -Install
#   .\recomp\scripts\build-mod.ps1 -Mod recomp\mods\hid_input_logger -Install `
#       -Config @{ force_buttons = '0x8000'; force_after_seconds = 60 }
#   .\recomp\scripts\build-mod.ps1 -Mod recomp\mods\hid_input_logger -Uninstall
#
# The DLL is left in <root>\mods-build\<id>\ (outside the repo), or in -OutDir.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Mod,
    [switch]$Install,
    [switch]$Uninstall,
    # Keys overridden in the "config" object of the installed mod.json.
    [hashtable]$Config = @{},
    # Mods root; defaults to suyu's (or PLA_RECOMP_MODS_DIR / SUYU_RECOMP_MODS_DIR when set).
    [string]$ModsRoot = '',
    # Build output folder; defaults to <PLA_RECOMP_ROOT>\mods-build\<id>.
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')

$modDir = (Resolve-Path -LiteralPath $Mod).Path
$manifestPath = Join-Path $modDir 'mod.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "no mod.json in $modDir" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$id = $manifest.id
$titleId = ([string]$manifest.title_id).ToUpperInvariant()
if (-not $id -or -not $manifest.library -or $titleId -notmatch '^[0-9A-F]{16}$') {
    throw 'mod.json needs "id", "library" and "title_id" (16 hex)'
}
if (-not $ModsRoot) {
    $ModsRoot = if ($env:PLA_RECOMP_MODS_DIR) { $env:PLA_RECOMP_MODS_DIR } elseif ($env:SUYU_RECOMP_MODS_DIR) { $env:SUYU_RECOMP_MODS_DIR } else { Join-Path $env:APPDATA 'suyu\recomp\mods' }
}
$installDir = Join-Path (Join-Path $ModsRoot $titleId) $id

if ($Uninstall) {
    if (Test-Path -LiteralPath $installDir) { Remove-Item -LiteralPath $installDir -Recurse -Force }
    Write-Host "uninstalled: $installDir"
    return
}

# API header: the repo copy, compared with the fork one when it exists.
$include = Join-Path $PSScriptRoot '..\mods\include'
$forkHeader = Join-Path $MK8R_ROOT_DIR 'third_party\suyu\src\core\arm\recomp\pla_mod_api.h'
if (Test-Path -LiteralPath $forkHeader) {
    $a = (Get-FileHash -LiteralPath $forkHeader).Hash
    $b = (Get-FileHash -LiteralPath (Join-Path $include 'pla_mod_api.h')).Hash
    if ($a -ne $b) { Write-Warning "recomp\mods\include\pla_mod_api.h differs from the fork copy ($forkHeader)" }
}

$out = if ($OutDir) { $OutDir } else { Join-Path $PLA_RECOMP_ROOT "mods-build\$id" }
New-Item -ItemType Directory -Force -Path $out | Out-Null
$dll = Join-Path $out $manifest.library
$sources = (Get-ChildItem -LiteralPath $modDir -Filter *.c | ForEach-Object { "`"$($_.FullName)`"" }) -join ' '
if (-not $sources) { throw "no .c sources in $modDir" }
$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$cmd = "`"$clang`" /nologo /O2 /W3 /LD /I `"$((Resolve-Path $include).Path)`" $sources /Fe`"$dll`" /Fo`"$out\\`""
$log = Join-Path $out 'build.log'
& cmd.exe /c "call `"$env:MK8R_VCVARS`" >nul 2>&1 && $cmd > `"$log`" 2>&1"
$rc = $LASTEXITCODE
Get-Content -LiteralPath $log | Out-Host
if ($rc -ne 0 -or -not (Test-Path -LiteralPath $dll)) { throw "build of $id failed (exit $rc)" }
Write-Host ("built: {0} ({1:n0} bytes)" -f $dll, (Get-Item -LiteralPath $dll).Length)

if ($Install) {
    New-Item -ItemType Directory -Force -Path $installDir | Out-Null
    Copy-Item -LiteralPath $dll -Destination $installDir -Force
    if ($Config.Count -gt 0) {
        foreach ($k in $Config.Keys) {
            $manifest.config | Add-Member -NotePropertyName $k -NotePropertyValue $Config[$k] -Force
        }
        $json = $manifest | ConvertTo-Json -Depth 10
        [IO.File]::WriteAllText((Join-Path $installDir 'mod.json'), $json, (New-Object Text.UTF8Encoding $false))
    } else {
        Copy-Item -LiteralPath $manifestPath -Destination $installDir -Force
    }
    Write-Host "installed in $installDir"
}
