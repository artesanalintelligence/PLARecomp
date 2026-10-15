# Automated run of plahost's PLA.exe (test queue job helper), holding the game lock.
#
#   .\tools\port\plahost\run-host.ps1 [-Dist D:\PLARecomp\host\dist] [-Seconds 90] [-ModsDir <dir>]
#        [-Env @{ VAR = 'value' }] [-Owner HOST] [-Out <dir>]
#
# Starts the exe, waits, asks it to close (WM_CLOSE: the host shuts the mods down and exits),
# copies user\log\pla.log (and any crash/thread reports) to -Out and summarizes: exit code,
# first frame, median FPS, warnings/errors.
[CmdletBinding()]
param(
    [string]$Dist = 'D:\PLARecomp\host\dist',
    [int]$Seconds = 90,
    [string]$ModsDir = '',
    [hashtable]$Env = @{},
    [string]$Owner = 'HOST',
    [string]$Out = 'D:\PLARecomp\host\logs',
    [string]$ExeArgs = ''
)
$ErrorActionPreference = 'Stop'
$lock = 'D:\PLARecomp\locks\game'
try { New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null }
catch {
    $age = (Get-Date) - (Get-Item $lock).CreationTime
    if ($age.TotalMinutes -lt 20) { Write-Host "Lock busy: $(Get-Content $lock\owner.txt -ErrorAction SilentlyContinue)"; exit 3 }
    Write-Host "Abandoned lock ($([int]$age.TotalMinutes) min): taking it"
}
Set-Content -Path "$lock\owner.txt" -Value "$Owner $(Get-Date -Format s) plahost"
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Force $Out | Out-Null
$logOut = Join-Path $Out "host-$stamp.log"
$proc = $null
try {
    Get-ChildItem (Join-Path $Dist 'user\log') -Filter '*.txt' -ErrorAction SilentlyContinue | Remove-Item -Force
    $env:NVNVK_OVERLAY_START = '0'
    if ($ModsDir) { $env:PLA_RECOMP_MODS_DIR = $ModsDir }
    foreach ($k in $Env.Keys) { Set-Item -Path "env:$k" -Value $Env[$k] }
    $exe = Join-Path $Dist 'PLA.exe'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    if ($ExeArgs) { $proc = Start-Process -FilePath $exe -ArgumentList $ExeArgs -WorkingDirectory $Dist -PassThru }
    else { $proc = Start-Process -FilePath $exe -WorkingDirectory $Dist -PassThru }
    $windowAt = $null
    while ($sw.Elapsed.TotalSeconds -lt $Seconds -and -not $proc.HasExited) {
        Start-Sleep -Milliseconds 250
        if (-not $windowAt) { $proc.Refresh(); if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $windowAt = $sw.Elapsed.TotalSeconds } }
    }
    $early = $proc.HasExited
    $title = ''
    $closeSec = $null
    if (-not $proc.HasExited) {
        $proc.Refresh(); $title = $proc.MainWindowTitle
        $c = [Diagnostics.Stopwatch]::StartNew()
        $proc.CloseMainWindow() | Out-Null
        if ($proc.WaitForExit(20000)) { $closeSec = $c.Elapsed.TotalSeconds } else { Stop-Process -Id $proc.Id -Force }
    }
    Start-Sleep -Milliseconds 500
    Copy-Item (Join-Path $Dist 'user\log\pla.log') $logOut -ErrorAction SilentlyContinue
    foreach ($r in Get-ChildItem (Join-Path $Dist 'user\log') -Filter '*.txt' -ErrorAction SilentlyContinue) {
        Copy-Item $r.FullName (Join-Path $Out "host-$stamp-$($r.Name)") -Force
        Write-Host "report: $($r.Name)"
    }
    $code = if ($proc.HasExited) { $proc.ExitCode } else { 'killed' }
    Write-Host ("early_exit={0} exit_code={1} close_s={2} window_at={3:n1}s title='{4}'" -f $early, $code, $closeSec, $windowAt, $title)
    Write-Host "log: $logOut"
    if (Test-Path $logOut) {
        $lines = Get-Content $logOut
        $fps = $lines | Where-Object { $_ -match 'PERF game_fps=([0-9.]+)' } | ForEach-Object { [double]([regex]::Match($_, 'game_fps=([0-9.]+)').Groups[1].Value) }
        $first = $lines | Where-Object { $_ -match 'PERF game_fps=([1-9][0-9.]*)' } | Select-Object -First 1
        if ($first) { Write-Host "  first FPS>0: $first" }
        $nz = @($fps | Where-Object { $_ -gt 0 } | Sort-Object)
        if ($nz.Count) { Write-Host ("  FPS: n={0} median={1:n1} min={2:n1} max={3:n1}" -f $nz.Count, $nz[[int]($nz.Count / 2)], $nz[0], $nz[-1]) }
        $crit = @($lines | Where-Object { $_ -match '<Critical>' }).Count
        $errs = @($lines | Where-Object { $_ -match '<Error>' }).Count
        Write-Host "  critical=$crit errors=$errs"
        $lines | Where-Object { $_ -match '<Critical>|<Error>|<Warning>' } | Select-Object -First 40 | ForEach-Object { Write-Host "  $_" }
    }
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $lock -ErrorAction SilentlyContinue
}
