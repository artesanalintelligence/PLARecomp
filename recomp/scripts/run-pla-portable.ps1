# Starts PLA in the portable suyu (setup-portable-suyu.ps1) unattended, through MCP,
# with its own port and its own user folder. Unlike run-pla.ps1 (which wraps
# mk8-recomp's run-hybrid.ps1) it does not kill any other suyu.exe.
#
#   .\recomp\scripts\run-pla-portable.ps1 -Mode hybrid -RecompDir D:\PLARecomp\mk8-recomp\build\recomp\pla `
#       -Seconds 120 -Screenshots 50,80
#   .\recomp\scripts\run-pla-portable.ps1 -Mode jit -Seconds 90
#
# Writes to D:\PLARecomp\logs\portable-<mode>-<date>.*: output, suyu_log.txt,
# recomp_gaps.json and the .shot-<s>.png screenshots (seconds since startup).

[CmdletBinding()]
param(
    [ValidateSet('jit', 'hybrid')][string]$Mode = 'hybrid',
    [int]$Seconds = 120,
    [string]$RecompDir = '',
    [string]$Rom = '',
    [string]$SuyuDir = '',
    [int]$McpPort = 9762,
    [int[]]$Screenshots = @(),
    [switch]$Unlimited,
    # Extra environment variables for suyu, e.g. @{ SUYU_RECOMP_MODS = '0' }.
    [hashtable]$ExtraEnv = @{}
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
if (-not $Rom) { $Rom = $PLA_BASE_NSP }
if (-not $Rom) { throw 'no base game NSP: pass -Rom (none found in the repo dump\ folder)' }
if (-not $SuyuDir) { $SuyuDir = Join-Path $PLA_RECOMP_ROOT 'suyu-portable' }
$exe = Join-Path $SuyuDir 'suyu.exe'
$user = Join-Path $SuyuDir 'user'
if (-not (Test-Path -LiteralPath $exe) -or -not (Test-Path -LiteralPath $user)) {
    throw "no portable suyu in $SuyuDir (setup-portable-suyu.ps1)"
}
$logDir = Join-Path $PLA_RECOMP_ROOT 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$prefix = Join-Path $logDir "portable-$Mode-$stamp"
$mcpDir = Join-Path $MK8R_ROOT_DIR 'scripts'

function Test-Port { (Test-NetConnection 127.0.0.1 -Port $McpPort -WarningAction SilentlyContinue).TcpTestSucceeded }
if (Test-Port) { throw "MCP port $McpPort is already in use" }
$env:SUYU_MCP_PORT = [string]$McpPort

if ($Mode -eq 'hybrid') {
    if (-not $RecompDir) { throw '-RecompDir is required in hybrid mode' }
    $env:SUYU_RECOMP_DIR = [IO.Path]::GetFullPath($RecompDir)
} else {
    Remove-Item Env:\SUYU_RECOMP_DIR -ErrorAction SilentlyContinue
}
foreach ($k in $ExtraEnv.Keys) { Set-Item -Path "Env:$k" -Value ([string]$ExtraEnv[$k]) }

# qt-config.ini is rewritten on exit: the limiter is set on every run.
$cfg = Join-Path $user 'config\qt-config.ini'
if (Test-Path -LiteralPath $cfg) {
    $text = Get-Content -LiteralPath $cfg -Raw
    $limit = if ($Unlimited) { 'false' } else { 'true' }
    $text = $text -replace 'use_speed_limit\\default=(true|false)', "use_speed_limit\default=$limit"
    $text = $text -replace 'use_speed_limit=(true|false)', "use_speed_limit=$limit"
    $text = $text -replace 'use_vsync\\default=(true|false)', $(if ($Unlimited) { 'use_vsync\default=false' } else { 'use_vsync\default=true' })
    $text = $text -replace 'use_vsync=\d+', $(if ($Unlimited) { 'use_vsync=0' } else { 'use_vsync=2' })
    # With the GDB stub enabled (config copied from another setup) the process waits for a
    # debugger and the game never starts.
    $text = $text -replace 'use_gdbstub\\default=(true|false)', 'use_gdbstub\default=true'
    $text = $text -replace 'use_gdbstub=(true|false)', 'use_gdbstub=false'
    [IO.File]::WriteAllText($cfg, $text, (New-Object Text.UTF8Encoding $false))
}
$suyuLog = Join-Path $user 'log\suyu_log.txt'
if (Test-Path -LiteralPath $suyuLog) { Remove-Item -LiteralPath $suyuLog -Force }

$py = Join-Path $env:TEMP "boot-pla-$PID.py"
Set-Content -LiteralPath $py -Encoding ascii -Value @"
import json, sys, time
sys.path.insert(0, r'$mcpDir')
from mcp import rpc
def call(name, args=None, timeout=300.0):
    out = rpc('tools/call', {'name': name, 'arguments': args or {}}, timeout=timeout)
    for b in out.get('result', {}).get('content', []):
        if b.get('type') == 'text':
            try: return json.loads(b['text'])
            except ValueError: return b['text']
    return out.get('error', out)
rom, seconds, prefix, t0 = sys.argv[1], int(sys.argv[2]), sys.argv[3], float(sys.argv[4])
shots = sorted(int(s) for s in sys.argv[5].split(',') if s)
print('launch', json.dumps(call('launch_game_path', {'path': rom})), flush=True)
start = time.time()
next_poll = start + 10
while time.time() - start < seconds:
    now = time.time()
    while shots and now - t0 >= shots[0]:
        s = shots.pop(0)
        print('shot %d s' % s, json.dumps(call('capture_game_screenshot', {'path': '%s.shot-%d.png' % (prefix, s)}, 60.0)), flush=True)
    if now >= next_poll:
        st = call('get_emulator_state', timeout=30.0)
        print('%5.0f s  running=%s first_frame=%s fps=%s' % (now - t0, st.get('game_running'), st.get('first_frame_displayed'), st.get('fps')), flush=True)
        next_poll += 10
    time.sleep(0.5)
print('stop', json.dumps(call('stop_emulation', timeout=120.0)), flush=True)
time.sleep(8)
"@

$p = Start-Process -FilePath $exe -ArgumentList '-hacker' -PassThru
# The "Firmware Not Found" modal blocks launch_game_path: a job accepts it, only in
# this process (another suyu may be open).
$clicker = Start-Job -ArgumentList (Join-Path $PSScriptRoot 'click-suyu-button.ps1'), $p.Id -ScriptBlock {
    param($script, $procId)
    $deadline = (Get-Date).AddMinutes(4)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        $r = & $script -Button 'Yes' -WindowText 'Firmware Not Found' -ProcessId $procId 2>$null
        if ($LASTEXITCODE -eq 0) { $r; break }
    }
}
$t0 = [DateTimeOffset]::Now.ToUnixTimeMilliseconds() / 1000.0
try {
    $deadline = (Get-Date).AddMinutes(3)
    do { Start-Sleep 2; if ($p.HasExited) { throw "suyu exited while starting ($($p.ExitCode))" } }
    until ((Test-Port) -or (Get-Date) -ge $deadline)
    if (-not (Test-Port)) { throw 'the suyu MCP server did not respond' }
    # python's stderr inside cmd: in PowerShell 5.1 with 'Stop' every line would be an exception.
    $shots = if ($Screenshots.Count) { $Screenshots -join ',' } else { '""' }
    & cmd.exe /c "python `"$py`" `"$Rom`" $Seconds `"$prefix`" $t0 $shots 2>&1" |
        Tee-Object -FilePath "$prefix.log" | Out-Host
} finally {
    Remove-Item -LiteralPath $py -Force -ErrorAction SilentlyContinue
    Receive-Job $clicker -ErrorAction SilentlyContinue | Out-Host
    Remove-Job $clicker -Force
    if (-not $p.HasExited) {
        $null = $p.CloseMainWindow()
        if (-not $p.WaitForExit(240000)) { Write-Warning 'suyu did not close in 240 s; killing it'; $p.Kill() }
    }
    Start-Sleep 3
    foreach ($f in 'suyu_log.txt', 'recomp_gaps.json') {
        $src = Join-Path $user "log\$f"
        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination "$prefix.$f" -Force }
    }
}
$log = "$prefix.suyu_log.txt"
if (Test-Path -LiteralPath $log) {
    Write-Host '=== recomp / mods ==='
    Select-String -LiteralPath $log -Pattern 'recomp mods|\[mod |static blocks|JIT ->|lookup miss|transitions|unimplemented opcode|SVCs to HLE|blocks per transition' |
        ForEach-Object { $_.Line } | Select-Object -Last 60
}
