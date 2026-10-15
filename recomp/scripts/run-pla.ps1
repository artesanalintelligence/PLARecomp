# Starts PLA in suyu unattended (through MCP), lets it run and closes it cleanly.
#
# Wraps mk8-recomp's scripts\run-hybrid.ps1:
#   -Mode jit     -> run-hybrid.ps1 -Baseline (Dynarmic only)
#   -Mode hybrid  -> SUYU_RECOMP_DIR = <RecompDir> (recompiled_*.dll DLLs + fallback JIT)
# When done it copies suyu's log and recomp_gaps.json to D:\PLARecomp\logs.
#
# Careful: run-hybrid.ps1 kills any open suyu.exe before starting. That is why this
# script refuses to start if a suyu is already running (another run or the user),
# unless -Force is given.
#
# -Screenshots 40,70: PNG screenshots of the game (MCP capture_game_screenshot) at those
# seconds since suyu started, in D:\PLARecomp\logs\run-<mode>-<date>.shot-<s>.png.
#
#   .\recomp\scripts\run-pla.ps1 -Mode jit -Seconds 120
#   .\recomp\scripts\run-pla.ps1 -Mode hybrid -RecompDir D:\PLARecomp\mk8-recomp\build\recomp\pla -Seconds 300

[CmdletBinding()]
param(
    [ValidateSet('jit', 'hybrid')][string]$Mode = 'jit',
    [int]$Seconds = 120,
    [string]$RecompDir = '',
    [string]$Rom = '',
    [switch]$Unlimited,
    [int[]]$Screenshots = @(),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
if (-not $Rom) { $Rom = $PLA_BASE_NSP }
if (-not $Rom) { throw 'no base game NSP: pass -Rom (none found in the repo dump\ folder)' }
$logDir = Join-Path $PLA_RECOMP_ROOT 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logDir "run-$Mode-$stamp.log"
if (-not $Force -and (Get-Process suyu -ErrorAction SilentlyContinue)) {
    throw 'a suyu.exe is already running; run-hybrid.ps1 would kill it. Wait or use -Force.'
}

$argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
             (Join-Path $MK8R_ROOT_DIR 'scripts\run-hybrid.ps1'),
             '-Root', "`"$MK8R_ROOT_DIR`"", '-Rom', "`"$Rom`"", '-RunSeconds', $Seconds)
# Start-Process does not quote arguments with spaces: they are quoted by hand.
if ($Mode -eq 'jit') {
    $argList += '-Baseline'
} else {
    if (-not $RecompDir) { throw '-RecompDir is required in hybrid mode' }
    # run-hybrid.ps1 builds the path as <Root>\build\recomp\<Target>.
    $base = Join-Path $MK8R_ROOT_DIR 'build\recomp'
    $full = [IO.Path]::GetFullPath($RecompDir)
    if (-not $full.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)) {
        throw "RecompDir must be under $base"
    }
    $argList += @('-Target', $full.Substring($base.Length + 1))
}
if ($Unlimited) { $argList += '-Unlimited' }

# Without firmware installed, LoadROM shows the "Firmware Not Found" modal and the MCP
# call launch_game_path stays blocked. A separate job accepts it ("Yes") as soon as it appears.
$clicker = Start-Job -ArgumentList (Join-Path $PSScriptRoot 'click-suyu-button.ps1') -ScriptBlock {
    param($script)
    $deadline = (Get-Date).AddMinutes(4)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        if (Get-Process suyu -ErrorAction SilentlyContinue) {
            $r = & $script -Button 'Yes' -WindowText 'Firmware Not Found' 2>$null
            if ($LASTEXITCODE -eq 0) { $r; break }
        }
    }
}

$shooter = $null
if ($Screenshots.Count -gt 0) {
    $shooter = Start-Job -ArgumentList (Join-Path $MK8R_ROOT_DIR 'scripts'), $Screenshots, `
            ($out -replace '\.log$', '') -ScriptBlock {
        param($mcpDir, $times, $prefix)
        $code = @"
import json, sys
sys.path.insert(0, r'$mcpDir')
from mcp import rpc
print(json.dumps(rpc('tools/call', {'name': 'capture_game_screenshot', 'arguments': {'path': sys.argv[1]}}, timeout=60.0)))
"@
        $py = [IO.Path]::GetTempFileName() + '.py'
        Set-Content -LiteralPath $py -Value $code -Encoding ascii
        $t0 = Get-Date
        foreach ($t in ($times | Sort-Object)) {
            $wait = $t - ((Get-Date) - $t0).TotalSeconds
            if ($wait -gt 0) { Start-Sleep -Seconds $wait }
            $png = "$prefix.shot-$t.png"
            "screenshot $t s: " + (& python $py $png 2>&1 | Out-String).Trim()
        }
        Remove-Item -LiteralPath $py -Force -ErrorAction SilentlyContinue
    }
}

$p = Start-Process powershell.exe -ArgumentList $argList -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError "$out.err"
Receive-Job $clicker -ErrorAction SilentlyContinue | Out-Host
Remove-Job $clicker -Force
if ($shooter) {
    Wait-Job $shooter -Timeout 30 | Out-Null
    Receive-Job $shooter -ErrorAction SilentlyContinue | Out-Host
    Remove-Job $shooter -Force
}
Write-Host "run-hybrid.ps1 ($Mode): exit $($p.ExitCode). Output: $out"

$suyuLogDir = Join-Path $env:APPDATA 'suyu\log'
foreach ($f in 'suyu_log.txt', 'recomp_gaps.json') {
    $src = Join-Path $suyuLogDir $f
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $logDir "run-$Mode-$stamp.$f") -Force
    }
}
Get-Content -LiteralPath $out | Select-Object -Last 40
exit $p.ExitCode
