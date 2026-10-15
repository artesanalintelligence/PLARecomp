# Builds suyu (mk8-recomp fork) with the environment of recomp\scripts\env.ps1.
#
# Calls mk8-recomp's scripts\build-suyu.ps1 in a separate process with its output
# redirected at the process level. Redirecting it inside PowerShell 5.1 (*>&1 or
# 2>&1) turns every cmake stderr line (e.g. a "CMake Deprecation Warning") into
# an ErrorRecord, and with $ErrorActionPreference='Stop' that aborts the build.
#
#   .\recomp\scripts\build-suyu-pla.ps1            # suyu + suyu-cmd, with JIT
#   .\recomp\scripts\build-suyu-pla.ps1 -NoJit     # build without Dynarmic (strict AOT)

[CmdletBinding()]
param([switch]$NoJit, [switch]$Configure, [switch]$Clean)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$logDir = Join-Path $PLA_RECOMP_ROOT 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$tag = if ($NoJit) { 'nojit' } else { 'jit' }
$out = Join-Path $logDir "build-suyu-$tag.log"
$err = Join-Path $logDir "build-suyu-$tag.err.log"

$argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
             (Join-Path $MK8R_ROOT_DIR 'scripts\build-suyu.ps1'), '-Root', $MK8R_ROOT_DIR)
if ($NoJit)     { $argList += '-NoJit' }
if ($Configure) { $argList += '-Configure' }
if ($Clean)     { $argList += '-Clean' }

$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process powershell.exe -ArgumentList $argList -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError $err
$sw.Stop()
Write-Host ("build-suyu ($tag): exit {0} in {1:n1} min. Log: {2}" -f $p.ExitCode, $sw.Elapsed.TotalMinutes, $out)
exit $p.ExitCode
