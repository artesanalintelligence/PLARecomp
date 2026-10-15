# Exports PLA 1.1.1 with suyu's AOT exporter, without clicks, through its MCP server.
#
# Flow (same as mk8-recomp's scripts/export-static-title.ps1, but with a selectable
# backend and timing):
#   1. Starts suyu.exe -hacker (the MCP server listens on 127.0.0.1:$McpPort).
#   2. trigger_ui_action{export_game}  -> opens GameExportDialog (modal; does not reply).
#   3. trigger_ui_action{aot_test_export, rom_path, output_dir, format, backend}
#      -> TriggerExportForTesting: real export, without update/firmware dialogs.
#   4. Polls get_aot_export_status until done, then closes suyu.
#
# Update 1.1.1 must be installed in suyu's NAND first
# (recomp\scripts\stage-suyu-userdata.py --install-update ...): the exporter
# applies the update with PatchManager::PatchExeFS; the ROM passed to it is the base game.
#
#   . .\recomp\scripts\env.ps1
#   .\recomp\scripts\export-pla.ps1 -Backend hybrid -Format source

[CmdletBinding()]
param(
    [ValidateSet('hybrid', 'static', 'dynarmic')][string]$Backend = 'hybrid',
    [ValidateSet('source', 'build')][string]$Format = 'source',
    [string]$Rom = '',
    [string]$OutDir = '',
    [string]$SuyuExe = '',
    [int]$McpPort = 9742,
    [int]$TimeoutMinutes = 240,
    # Folder with <module>.roots (hex offsets relative to .text) -> SUYU_AOT_EXTRA_ROOTS.
    [string]$Roots = '',
    [string]$Tag = '',
    # Deletes exefs\romfs.bin (6 GB) when done: not needed to measure coverage or build
    # the hybrid DLLs, because hybrid mode boots from the NSP.
    [switch]$DropRomfs,
    # Translates SHL by immediate (SUYU_AOT_SHL_IMM=1, pla-mods branch of the fork): it is the
    # only PLA 1.1.1 instruction the Hybrid export used to leave to the JIT.
    [switch]$ShlImm
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
if (-not $Rom)     { $Rom = $PLA_BASE_NSP }
if (-not $Rom)     { throw 'no base game NSP: pass -Rom (none found in the repo dump\ folder)' }
if (-not $Tag)     { $Tag = "$Backend-$Format" + $(if ($Roots) { '-roots' } else { '' }) }
if (-not $OutDir)  { $OutDir = Join-Path $PLA_RECOMP_ROOT "exports\pla-$Tag" }
if ($Roots) { $env:SUYU_AOT_EXTRA_ROOTS = [IO.Path]::GetFullPath($Roots) } else { Remove-Item Env:\SUYU_AOT_EXTRA_ROOTS -ErrorAction SilentlyContinue }
if (-not $SuyuExe) { $SuyuExe = Join-Path $MK8R_ROOT_DIR 'build\suyu\bin\suyu.exe' }
if ($ShlImm) { $env:SUYU_AOT_SHL_IMM = '1' } else { Remove-Item Env:\SUYU_AOT_SHL_IMM -ErrorAction SilentlyContinue }
# A portable suyu (setup-portable-suyu.ps1) uses <exe>\user instead of %APPDATA%\suyu.
$suyuUser = Join-Path (Split-Path -Parent $SuyuExe) 'user'
if (-not (Test-Path -LiteralPath $suyuUser)) { $suyuUser = Join-Path $env:APPDATA 'suyu' }
$mcpCall = Join-Path $MK8R_ROOT_DIR 'scripts\mcp-call.py'
$mcpPy   = Join-Path $MK8R_ROOT_DIR 'scripts'
$logDir  = Join-Path $PLA_RECOMP_ROOT 'logs'
New-Item -ItemType Directory -Force -Path $OutDir, $logDir | Out-Null
if ($OutDir.Length -gt 70) { Write-Warning "OutDir has $($OutDir.Length) characters; the exporter recommends <= 70" }

function Test-Port { (Test-NetConnection 127.0.0.1 -Port $McpPort -WarningAction SilentlyContinue).TcpTestSucceeded }
function Invoke-Mcp([string]$Name, [hashtable]$Arguments = @{}) {
    $argv = @($mcpCall, $Name)
    foreach ($k in $Arguments.Keys) { $argv += "--$k"; $argv += [string]$Arguments[$k] }
    $raw = & python @argv
    if ($LASTEXITCODE -ne 0) { throw "MCP $Name failed: $($raw -join "`n")" }
    ($raw -join "`n") | ConvertFrom-Json
}
# Call that does not return (the handler stays blocked in the modal's exec()).
function Send-McpNoWait([string]$JsonArgs) {
    $code = @"
import json, os, sys, socket
sys.path.insert(0, r'$mcpPy')
from mcp import rpc
try:
    print(json.dumps(rpc('tools/call', {'name': 'trigger_ui_action', 'arguments': json.loads(os.environ['MCP_ARGS'])}, timeout=8.0)))
except (TimeoutError, socket.timeout):
    print('blocked (expected)')
"@
    $tmp = Join-Path $env:TEMP "mcp-nowait-$PID.py"
    Set-Content -LiteralPath $tmp -Value $code -Encoding ascii
    $env:MCP_ARGS = $JsonArgs
    & python $tmp
    Remove-Item -LiteralPath $tmp -Force
}

if (Test-Port) { throw "MCP port $McpPort is already in use (another suyu instance?)" }
$env:SUYU_MCP_PORT = [string]$McpPort
$suyuLog = Join-Path $suyuUser 'log\suyu_log.txt'
if (Test-Path -LiteralPath $suyuLog) { Remove-Item -LiteralPath $suyuLog -Force }

$p = Start-Process -FilePath $SuyuExe -ArgumentList '-hacker' -PassThru
try {
    $deadline = (Get-Date).AddMinutes(3)
    do { Start-Sleep 2; if ($p.HasExited) { throw "suyu exited while starting ($($p.ExitCode))" } }
    until ((Test-Port) -or (Get-Date) -ge $deadline)
    if (-not (Test-Port)) { throw 'the suyu MCP server did not respond' }

    Write-Host '[1/2] opening GameExportDialog'
    Send-McpNoWait '{"action":"export_game"}'
    Start-Sleep 4
    $exportArgs = @{ action = 'aot_test_export'; rom_path = $Rom; output_dir = $OutDir;
               format = $Format; backend = $Backend } | ConvertTo-Json -Compress
    Write-Host "[2/2] aot_test_export backend=$Backend format=$Format"
    Send-McpNoWait $exportArgs

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $last = ''
    do {
        Start-Sleep 15
        if ($p.HasExited) { throw "suyu exited during the export ($($p.ExitCode))" }
        try { $st = Invoke-Mcp get_aot_export_status } catch { Write-Warning $_; continue }
        $line = '{0,6:n1} min {1,3}%  {2}' -f $sw.Elapsed.TotalMinutes, $st.progress, $st.status
        if ($line -ne $last) { Write-Host $line; $last = $line }
    } until ($st.done -or (Get-Date) -ge $deadline)
    $sw.Stop()
    $st | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $logDir "export-$Tag.status.json")
    Write-Host ('export finished in {0:n1} min, success={1}' -f $sw.Elapsed.TotalMinutes, $st.success)
} finally {
    # Close the GameExportDialog (modal) first: with the modal open the main window
    # ignores WM_CLOSE, suyu ends up killed and the asynchronous logger loses the last
    # lines (precisely the rtld/sdk/subsdk* coverage ones).
    if (-not $p.HasExited) {
        try {
            Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
            $root = [Windows.Automation.AutomationElement]::RootElement
            $cond = New-Object Windows.Automation.PropertyCondition(
                [Windows.Automation.AutomationElement]::ProcessIdProperty, $p.Id)
            $winCond = New-Object Windows.Automation.PropertyCondition(
                [Windows.Automation.AutomationElement]::ControlTypeProperty,
                [Windows.Automation.ControlType]::Window)
            foreach ($w in $root.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
                foreach ($d in $w.FindAll([Windows.Automation.TreeScope]::Descendants, $winCond)) {
                    try { $d.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close() } catch { }
                }
            }
            Start-Sleep 3
        } catch { Write-Warning "could not close the dialog: $_" }
        $null = $p.CloseMainWindow()
        if (-not $p.WaitForExit(60000)) { Write-Warning 'suyu did not close in 60 s; killing it (the log may be truncated)'; $p.Kill() }
    }
    # The package folder is named "<game title> - Hybrid AOT + JIT": the non-ASCII characters
    # of the title break the .cmd/CMake files, which are written in ASCII. It is renamed to "pkg".
    # A "pkg" left by an earlier export (e.g. a re-export after a platform update, or a work folder cleaned with
    # --clean) is replaced by the fresh package; otherwise the new manifest would stay in the title folder.
    $fresh = Get-ChildItem -LiteralPath $OutDir -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match ' - (Hybrid|suyu|Dynarmic|Static)' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($fresh) {
        $pkg = Join-Path $OutDir 'pkg'
        if (Test-Path -LiteralPath $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
        Rename-Item -LiteralPath $fresh.FullName -NewName 'pkg'
    }
    if ($DropRomfs) {
        Get-ChildItem -LiteralPath $OutDir -Recurse -Filter romfs.bin -File -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host "deleting $($_.FullName) ($([math]::Round($_.Length/1GB,2)) GB)"; Remove-Item -LiteralPath $_.FullName -Force }
    }
    if (Test-Path -LiteralPath $suyuLog) {
        Copy-Item -LiteralPath $suyuLog -Destination (Join-Path $logDir "export-$Tag.suyu_log.txt") -Force
    }
}

# Figures: "AOT coverage" lines of the log and size of the generated C.
$log = Join-Path $logDir "export-$Tag.suyu_log.txt"
if (Test-Path -LiteralPath $log) {
    Select-String -LiteralPath $log -Pattern 'AOT coverage|AOT diag|EmitProject|fell back|Recompiled modules' |
        ForEach-Object { $_.Line } | Tee-Object -FilePath (Join-Path $logDir "export-$Tag.coverage.txt")
}
$c = Get-ChildItem -LiteralPath $OutDir -Recurse -File -Include *.c, *.h -ErrorAction SilentlyContinue |
     Measure-Object -Property Length -Sum
Write-Host ('generated C: {0} files, {1:n2} GB' -f $c.Count, ($c.Sum / 1GB))
