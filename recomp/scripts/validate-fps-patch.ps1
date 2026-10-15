# Validates a framerate patch in suyu (JIT or Hybrid) without leaving traces in the configuration.
#
# 1. Copies the mod folder to %APPDATA%\suyu\load\01001F5010DFA000\ (and removes the title's other mods).
# 2. With -Unlimited, disables suyu's speed limiter and vsync.
# 3. Runs run-pla-safe.py (does not close other suyu.exe) and logs the FPS every 10 s.
# 4. Restores qt-config.ini and the load\ folder as they were.
#
#   .\recomp\scripts\validate-fps-patch.ps1 -Mod 'recomp\patches\unlocked-fps\suyu\Unlocked FPS (PLA recomp)'
#   .\recomp\scripts\validate-fps-patch.ps1 -Mod 'recomp\patches\unlocked-fps\suyu\Unlocked FPS (PLA recomp)' -Unlimited
#   .\recomp\scripts\validate-fps-patch.ps1 -Mod '' -Unlimited          # no patch (reference)

[CmdletBinding()]
param(
    [string]$Mod = '',
    [ValidateSet('jit', 'hybrid')][string]$Mode = 'jit',
    [string]$RecompDir = '',
    [int]$Seconds = 120,
    [switch]$Unlimited
)

$ErrorActionPreference = 'Stop'
$tid = '01001F5010DFA000'
$cfg = Join-Path $env:APPDATA 'suyu\config\qt-config.ini'
$load = Join-Path $env:APPDATA "suyu\load\$tid"
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$bak = Join-Path $env:TEMP "pla-fps-$stamp"
New-Item -ItemType Directory -Force -Path $bak | Out-Null
Copy-Item -LiteralPath $cfg -Destination (Join-Path $bak 'qt-config.ini')
if (Test-Path -LiteralPath $load) { Copy-Item -Recurse -LiteralPath $load -Destination (Join-Path $bak 'load') }

try {
    New-Item -ItemType Directory -Force -Path $load | Out-Null
    Get-ChildItem -LiteralPath $load | Remove-Item -Recurse -Force
    if ($Mod) {
        $src = Resolve-Path -LiteralPath $Mod
        Copy-Item -Recurse -LiteralPath $src -Destination $load
        Write-Host "mod: $(Split-Path $src -Leaf)"
    } else {
        Write-Host 'no mod (reference)'
    }
    if ($Unlimited) {
        # Same as run-hybrid.ps1 -Unlimited (mk8-recomp).
        $t = [IO.File]::ReadAllText($cfg)
        $t = $t -replace 'use_speed_limit\\default=true', 'use_speed_limit\default=false' -replace 'use_speed_limit=true', 'use_speed_limit=false'
        $t = $t -replace 'use_vsync\\default=true', 'use_vsync\default=false' -replace 'use_vsync=\d+', 'use_vsync=0'
        [IO.File]::WriteAllText($cfg, $t, (New-Object Text.UTF8Encoding $false))
        Write-Host 'UNLIMITED: suyu speed limiter and vsync disabled'
    }
    $py = @((Join-Path $PSScriptRoot 'run-pla-safe.py'), '--seconds', $Seconds)
    if ($Mode -eq 'hybrid') { $py += @('--recomp-dir', $RecompDir) }
    python -u @py 2>&1 | ForEach-Object { "$_" }
    $suyuLog = Join-Path $env:APPDATA 'suyu\log\suyu_log.txt'
    if (Test-Path -LiteralPath $suyuLog) {
        Copy-Item -LiteralPath $suyuLog -Destination "D:\PLARecomp\logs\fps-$stamp.suyu_log.txt" -Force
    }
} finally {
    Copy-Item -LiteralPath (Join-Path $bak 'qt-config.ini') -Destination $cfg -Force
    if (Test-Path -LiteralPath $load) { Get-ChildItem -LiteralPath $load | Remove-Item -Recurse -Force }
    if (Test-Path -LiteralPath (Join-Path $bak 'load')) {
        Copy-Item -Recurse -Force -Path (Join-Path $bak 'load\*') -Destination $load
    }
    Write-Host "configuration and mods restored (backup in $bak)"
}
