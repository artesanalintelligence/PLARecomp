# Automated test of the dist's PLA.exe (or suyu-cmd.exe), holding the game lock.
#
#   .\recomp\scripts\run-pla-exe.ps1 [-Dist D:\PLARecomp\dist\PLA-PC-v0] [-Seconds 90]
#        [-Exe PLA.exe] [-ExeArgs '--nvnvk'] [-Capture] [-Env @{ VAR = 'value' }]
#
# Takes D:\PLARecomp\locks\game (if someone else holds it, exits with 3), starts the exe with
# SUYU_CMD_PERF_SAMPLE=1 (FPS per second in the log), waits, closes the process, copies
# the log to D:\PLARecomp\logs\pla-exe-<date>.log and summarizes: time to the window,
# to the first frame, median FPS and mod hooks.
[CmdletBinding()]
param(
    [string]$Dist = 'D:\PLARecomp\dist\PLA-PC-v0',
    [int]$Seconds = 90,
    [string]$Exe = 'PLA.exe',
    [string]$ExeArgs = '',
    [switch]$Capture,
    [hashtable]$Env = @{},
    [string]$Owner = 'RUNTIME',
    [double[]]$ScreenAt = @(),
    # Dist mods with a "disabled" file that are enabled only during this test.
    [string[]]$EnableMods = @(),
    # Records the machine's audio output (WASAPI loopback) from second ProbeAt.
    [int]$ProbeAt = 0,
    [int]$ProbeSeconds = 10
)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class PlaWin { [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); }
'@
$ErrorActionPreference = 'Stop'
$lock = 'D:\PLARecomp\locks\game'
try { New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null }
catch {
    $age = (Get-Date) - (Get-Item $lock).CreationTime
    if ($age.TotalMinutes -lt 20) { Write-Host "Lock busy: $(Get-Content $lock\owner.txt -ErrorAction SilentlyContinue)"; exit 3 }
    Write-Host "Abandoned lock ($([int]$age.TotalMinutes) min): taking it"
}
Set-Content -Path "$lock\owner.txt" -Value "$Owner $(Get-Date -Format s) $Exe"
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logOut = "D:\PLARecomp\logs\pla-exe-$stamp.log"
$capDir = "D:\PLARecomp\logs\pla-exe-$stamp-frames"
$proc = $null
$reenable = @()
try {
    foreach ($m in $EnableMods) {
        $flag = Join-Path $Dist "mods\01001F5010DFA000\$m\disabled"
        if (Test-Path $flag) { Remove-Item $flag; $reenable += $flag }
    }
    $env:SUYU_CMD_PERF_SAMPLE = '1'
    # Tests: without the nvnvk menu start screen (it covers the screenshots and blocks scripted input).
    # A job that wants to test it passes -Env @{ NVNVK_OVERLAY_START = '1' }.
    $env:NVNVK_OVERLAY_START = '0'
    if ($Capture) {
        $env:SUYU_CMD_CAPTURE_DIR = $capDir
        $env:SUYU_CMD_CAPTURE_FIRST_SEC = '20'
        $env:SUYU_CMD_CAPTURE_INTERVAL_SEC = '20'
    }
    foreach ($k in $Env.Keys) { Set-Item -Path "env:$k" -Value $Env[$k] }
    # plahost (PLA.exe) reads PLA_RECOMP_MODS_DIR/_MOD_SET/_MODS_FORCE/_CHAIN_BUDGET and PLA_NATIVE_SPIN_US; the fork
    # (PLA-fork.exe, suyu-cmd.exe) the same names with the SUYU_ prefix. Callers may pass either: set both.
    foreach ($n in 'RECOMP_MODS_DIR', 'RECOMP_MOD_SET', 'RECOMP_MODS_FORCE', 'RECOMP_CHAIN_BUDGET', 'NATIVE_SPIN_US') {
        foreach ($pair in @(@("PLA_$n", "SUYU_$n"), @("SUYU_$n", "PLA_$n"))) {
            if ($Env.ContainsKey($pair[0]) -and -not $Env.ContainsKey($pair[1])) { Set-Item -Path "env:$($pair[1])" -Value $Env[$pair[0]] }
        }
    }
    $exePath = Join-Path $Dist $Exe
    $sw = [Diagnostics.Stopwatch]::StartNew()
    if ($ExeArgs) { $proc = Start-Process -FilePath $exePath -ArgumentList $ExeArgs -WorkingDirectory $Dist -PassThru }
    else { $proc = Start-Process -FilePath $exePath -WorkingDirectory $Dist -PassThru }
    $windowAt = $null
    $shot = @{}
    $probe = $null
    $probeOut = "D:\PLARecomp\logs\pla-exe-$stamp-audio.txt"
    while ($sw.Elapsed.TotalSeconds -lt $Seconds -and -not $proc.HasExited) {
        Start-Sleep -Milliseconds 250
        if ($ProbeAt -gt 0 -and -not $probe -and $sw.Elapsed.TotalSeconds -ge $ProbeAt) {
            $probe = Start-Process -FilePath 'D:\PLARecomp\mods-build\nnshim_audio\loopback_probe.exe' `
                -ArgumentList "$ProbeSeconds" -NoNewWindow -PassThru -RedirectStandardOutput $probeOut
        }
        foreach ($t in @($ScreenAt)) {
            if ($t -gt 0 -and $sw.Elapsed.TotalSeconds -ge $t -and -not $shot[$t]) {
                $shot[$t] = $true
                $proc.Refresh(); $r = New-Object PlaWin+RECT
                if ([PlaWin]::GetWindowRect($proc.MainWindowHandle, [ref]$r) -and $r.R -gt $r.L) {
                    $bmp = New-Object Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
                    $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
                    $f = "D:\PLARecomp\logs\pla-exe-$stamp-screen-$t.png"; $bmp.Save($f); Write-Host "screen: $f"
                }
            }
        }
        if (-not $windowAt) {
            $proc.Refresh()
            if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $windowAt = $sw.Elapsed.TotalSeconds }
        }
    }
    if ($probe) { $probe.WaitForExit(($ProbeSeconds + 5) * 1000) | Out-Null; Get-Content $probeOut | ForEach-Object { Write-Host "  audio: $_" } }
    $title = if (-not $proc.HasExited) { $proc.Refresh(); $proc.MainWindowTitle } else { '' }
    $exited = $proc.HasExited
    if (-not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null
        if (-not $proc.WaitForExit(15000)) { Stop-Process -Id $proc.Id -Force }
    }
    Start-Sleep -Seconds 1
    # PLA.exe writes user\log\pla.log since 2026-10-09 (suyu_log.txt before, and in suyu-cmd.exe): the newest one.
    $runLog = Get-ChildItem (Join-Path $Dist 'user\log') -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in 'pla.log', 'suyu_log.txt' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($runLog) { Copy-Item $runLog.FullName $logOut -ErrorAction SilentlyContinue }
    Copy-Item (Join-Path $Dist 'user\log\recomp_coverage.txt') ($logOut -replace '\.log$', '.coverage.txt') -ErrorAction SilentlyContinue
    Write-Host ("exe={0} early_exit={1} window_visible_at={2:n2}s title='{3}'" -f $Exe, $exited, $windowAt, $title)
    Write-Host "log: $logOut"
    if (Test-Path $logOut) {
        $lines = Get-Content $logOut
        $pick = { param($re) $lines | Where-Object { $_ -match $re } | Select-Object -First 1 }
        foreach ($re in 'starting up', 'Udp Initialization', 'Joycon driver', 'Window created', 'system.Load returned', 'recomp mods: loaded', 'ArmRecomp::RunThread entered', 'PLA:') {
            $l = & $pick $re; if ($l) { Write-Host "  $l" }
        }
        $fps = $lines | Where-Object { $_ -match 'PERF game_fps=([0-9.]+)' } | ForEach-Object { [double]([regex]::Match($_, 'game_fps=([0-9.]+)').Groups[1].Value) }
        $first = $lines | Where-Object { $_ -match 'PERF game_fps=([1-9][0-9.]*)' } | Select-Object -First 1
        if ($first) { Write-Host "  first FPS>0: $first" }
        if ($fps) {
            $nz = @($fps | Where-Object { $_ -gt 0 } | Sort-Object)
            if ($nz.Count) { Write-Host ("  FPS: n={0} median={1:n1} min={2:n1} max={3:n1}" -f $nz.Count, $nz[[int]($nz.Count/2)], $nz[0], $nz[-1]) }
        }
        $lines | Where-Object { $_ -match 'was called|CRITICAL|Critical' } | Select-Object -First 15 | ForEach-Object { Write-Host "  $_" }
    }
    if ($Capture -and (Test-Path $capDir)) { Write-Host "captures: $capDir" }
}
finally {
    foreach ($f in $reenable) { New-Item -ItemType File -Path $f -Force | Out-Null }
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $lock -ErrorAction SilentlyContinue
}
