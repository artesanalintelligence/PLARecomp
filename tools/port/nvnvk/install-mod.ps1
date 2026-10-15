# Installs an nvnvk mod (tracer or backend) into the portable suyu (or -ModsRoot).
#   .\tools\port\nvnvk\install-mod.ps1 -Mod nvntrace [-All] [-DetailFrames '0-3,60-62'] [-MaxMb 1500]
#   .\tools\port\nvnvk\install-mod.ps1 -Mod nvnvk [-Mode shadow|exclusive] [-CaptureDir D:\x -CaptureEvery 600 -CaptureMax 5] [-Validation]
#   .\tools\port\nvnvk\install-mod.ps1 -Mod nvnvk -Uninstall
param(
    [ValidateSet('nvntrace', 'nvnvk')][string]$Mod = 'nvntrace',
    [string]$ModsRoot = 'D:\PLARecomp\suyu-portable\user\recomp\mods',
    [switch]$All,
    [string]$DetailFrames = '0-3,60-62,600-602,3000-3002',
    [int]$MaxMb = 1500,
    [string]$TraceDir = 'D:\PLARecomp\traces\nvn',
    [ValidateSet('shadow', 'exclusive')][string]$Mode = 'shadow',
    [string]$CaptureDir = '',
    [int]$CaptureEvery = 0,
    [int]$CaptureMax = 0,
    [switch]$Validation,
    [string]$CapturePath = '',
    [int]$CaptureFrames = 1200,
    [switch]$NoPresent,
    [string]$Extra = '',
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$dst = Join-Path $ModsRoot "01001F5010DFA000\$Mod"
if ($Uninstall) { if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }; return }
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$gen = @((Join-Path $PSScriptRoot 'gen_tables.py'), '--mod', $Mod, '--mod-json', $dst, '--trace-dir', $TraceDir,
         '--max-mb', $MaxMb, '--detail-frames', $DetailFrames, '--mode', $Mode,
         '--capture-every', $CaptureEvery, '--capture-max', $CaptureMax)
if ($CaptureDir) { $gen += @('--capture-dir', $CaptureDir) }
if ($All) { $gen += '--all' }
if ($Validation) { $gen += '--validation' }
if ($CapturePath) { $gen += @('--capture-path', $CapturePath, '--capture-frames', $CaptureFrames) }
if ($NoPresent) { $gen += '--no-present' }
if ($Extra) { $gen += @('--extra', $Extra) }
& python @gen | Out-Host
Copy-Item -Force (Join-Path $repo "build\port\nvnvk\$Mod.dll") $dst
Remove-Item -Force (Join-Path $dst 'disabled') -ErrorAction SilentlyContinue
Get-ChildItem $dst | Select-Object Name, Length | Out-Host
