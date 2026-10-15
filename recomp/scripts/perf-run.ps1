# PLA performance measurement run, reproducible and without touching the dist.
#
# Uses a copy of the dist (D:\PLARecomp\perf\dist: DLLs and exefs as hard links, its own user\),
# takes the D:\PLARecomp\locks\game lock, starts suyu-cmd with the given config and, optionally,
# a TAS script (automatic input without window focus). Samples the game FPS every second
# (SUYU_CMD_PERF_SAMPLE), the total system load and, if requested, a per-thread profile (hostprof).
#
#   .\recomp\scripts\perf-run.ps1 -Tag base-lim -Config sdl2-config.ini -Seconds 240 -Tas title-intro
#   .\recomp\scripts\perf-run.ps1 -Tag base-unl -Config sdl2-unlimited.ini -Profile 120,30
#
# Writes to D:\PLARecomp\perf\runs\<date>-<tag>\: suyu_log.txt, perf.csv (t, game_fps, system_fps,
# frametime, speed), load.csv (system load), screenshots and hostprof.txt.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Tag,
    [string]$Dist = 'D:\PLARecomp\perf\dist',
    [string]$Exe = 'suyu-cmd.exe',
    [string]$Config = 'sdl2-config.ini',
    [int]$Seconds = 180,
    # Name of a script recomp\scripts\perf-tas\<name>.txt (suyu TAS format) or ''.
    [string]$Tas = '',
    # TAS commands per wall-clock second (0 = one per presented frame, suyu-cmd's original mode).
    [int]$TasHz = 60,
    # hostprof profile: start and duration in seconds since startup (e.g. 120,30).
    [int[]]$Profile = @(),
    # hostprof sampling interval in ms (1 = dense profile, e.g. for the link order).
    [int]$ProfileIntervalMs = 2,
    [int]$CaptureFirst = 0,
    [int]$CaptureEvery = 0,
    [hashtable]$ExtraEnv = @{},
    [switch]$NoLock,
    # Seconds to wait for another run to release locks\game.
    [int]$WaitLock = 0,
    # Folder with the PDB matching the executable (for hostprof).
    [string]$SymPath = ''
)
$ErrorActionPreference = 'Stop'
$root = 'D:\PLARecomp'
$lock = Join-Path $root 'locks\game'
if (-not $NoLock) {
    if (Test-Path (Join-Path $root 'locks\heavy')) {
        $o = Get-Content (Join-Path $root 'locks\heavy\owner.txt') -ErrorAction SilentlyContinue
        if ($o -notmatch '^PERF') { throw "locks\heavy exists ($o): the game is not run" }
    }
    $deadline = (Get-Date).AddSeconds($WaitLock)
    while ($true) {
        try { New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null; break }
        catch {
            if ((Get-Date) -gt $deadline) { throw "locks\game is held by someone else: $(Get-Content (Join-Path $lock 'owner.txt') -ErrorAction SilentlyContinue)" }
            Start-Sleep -Seconds 10
        }
    }
    "PERF $(Get-Date -Format s) $Tag" | Set-Content (Join-Path $lock 'owner.txt')
}
$proc = $null
try {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $out = Join-Path $root "perf\runs\$stamp-$Tag"
    New-Item -ItemType Directory -Force $out | Out-Null
    $user = Join-Path $Dist 'user'
    $tasDir = Join-Path $user 'tas'
    New-Item -ItemType Directory -Force $tasDir | Out-Null
    $argList = @('-V', '262144:1.1.1', '-c', "`"$(Join-Path $user "config\$Config")`"")
    if ($Tas) {
        Copy-Item (Join-Path $PSScriptRoot "perf-tas\$Tas.txt") (Join-Path $tasDir 'script0-1.txt') -Force
        $argList += '-t'
    }
    $env:SUYU_RECOMP_MODS_DIR = Join-Path $Dist 'mods'   # fork
    $env:PLA_RECOMP_MODS_DIR = $env:SUYU_RECOMP_MODS_DIR  # plahost
    $env:SUYU_CMD_PERF_SAMPLE = '1'
    $env:SUYU_CMD_TAS_HZ = [string]$TasHz
    if ($CaptureEvery -gt 0) {
        $env:SUYU_CMD_CAPTURE_DIR = $out
        $env:SUYU_CMD_CAPTURE_FIRST_SEC = [string]$CaptureFirst
        $env:SUYU_CMD_CAPTURE_INTERVAL_SEC = [string]$CaptureEvery
    } else { Remove-Item Env:\SUYU_CMD_CAPTURE_DIR -ErrorAction SilentlyContinue }
    foreach ($k in $ExtraEnv.Keys) { Set-Item -Path "Env:$k" -Value ([string]$ExtraEnv[$k]) }
    $log = Join-Path $user 'log\suyu_log.txt'
    Remove-Item $log -ErrorAction SilentlyContinue
    $exePath = Join-Path $Dist $Exe
    $proc = Start-Process -FilePath $exePath -ArgumentList $argList -WorkingDirectory $Dist -PassThru
    "pid $($proc.Id) $exePath $($argList -join ' ')" | Set-Content (Join-Path $out 'cmd.txt')
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $loadRows = @('t,cpu_total_pct,suyu_cpu_pct,other_top')
    $profDone = $false
    $lastCpu = 0.0; $lastT = 0.0
    while ($sw.Elapsed.TotalSeconds -lt $Seconds -and -not $proc.HasExited) {
        Start-Sleep -Seconds 5
        $t = [math]::Round($sw.Elapsed.TotalSeconds)
        $cpu = (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
        $proc.Refresh()
        $pc = $proc.TotalProcessorTime.TotalSeconds
        $suyuPct = if ($t -gt $lastT) { [math]::Round(100 * ($pc - $lastCpu) / ($t - $lastT) / [Environment]::ProcessorCount, 1) } else { 0 }
        $lastCpu = $pc; $lastT = $t
        $others = Get-Process | Where-Object { $_.Id -ne $proc.Id -and $_.CPU } |
            Sort-Object CPU -Descending | Select-Object -First 3 | ForEach-Object { "$($_.Name)" }
        $loadRows += "$t,$cpu,$suyuPct,$($others -join ' ')"
        if ($Profile.Count -eq 2 -and -not $profDone -and $t -ge $Profile[0]) {
            $profDone = $true
            # With 'hang' in the tag, blocked threads are sampled too (hang diagnosis).
            if ($Tag -match 'hang') { $env:HOSTPROF_ALL = '1' } else { Remove-Item Env:\HOSTPROF_ALL -ErrorAction SilentlyContinue }
            & (Join-Path $root 'perf\tools\hostprof.exe') $proc.Id $Profile[1] (Join-Path $out 'hostprof.txt') $ProfileIntervalMs 24 30 $(if ($SymPath) { $SymPath } else { $Dist })
        }
    }
} finally {
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
        Start-Sleep -Seconds 2
    }
    Get-Process suyu-cmd* -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Dist*" } | Stop-Process -Force
    if (-not $NoLock) { Remove-Item -Recurse -Force $lock -ErrorAction SilentlyContinue }
    # The -ExtraEnv variables must not leak into the next run of the same session.
    foreach ($k in $ExtraEnv.Keys) { Remove-Item -Path "Env:$k" -ErrorAction SilentlyContinue }
}
$loadRows | Set-Content (Join-Path $out 'load.csv')
Copy-Item $log $out -ErrorAction SilentlyContinue
Copy-Item (Join-Path $user 'log\recomp_coverage.txt') $out -ErrorAction SilentlyContinue
# FPS per second from the log.
$rows = @('t,game_fps,system_fps,frametime_ms,speed')
foreach ($l in Get-Content (Join-Path $out 'suyu_log.txt') -ErrorAction SilentlyContinue) {
    if ($l -match '^\[\s*([\d.]+)\].*PERF game_fps=([\d.]+) system_fps=([\d.]+) frametime_ms=([\d.]+) speed=([\d.]+)') {
        $rows += "$($Matches[1]),$($Matches[2]),$($Matches[3]),$($Matches[4]),$($Matches[5])"
    }
}
$rows | Set-Content (Join-Path $out 'perf.csv')
"output: $out ($($rows.Count - 1) FPS samples)"
