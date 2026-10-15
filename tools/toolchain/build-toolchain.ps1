# Builds the portable toolchain that compiles the emitted C into recompiled_*.dll without
# VS Build Tools, the Windows SDK or CMake (see docs/port/TOOLCHAIN.md).
#
# Package contents (one folder, nothing installed, PATH untouched):
#   bin\clang-cl.exe, bin\lld-link.exe     official LLVM binaries (Apache-2.0 with LLVM exception)
#   bin\llvm-objdump.exe                   official LLVM binary: AArch64 disassembly for the islands generator (nlift.py)
#   lib\clang\<N>\include\                  clang resource headers (freestanding C + x86 only)
#   include\                                our minimal headers (tools/toolchain/include)
#   lib\*.lib                               our import libraries, generated from def\*.def
#   lib\recomp_crt0.obj                     our minimal DLL startup (crt\recomp_crt0.c)
#   lib\recomp_isa_guard_v2/v3.obj         CPU level check for --isa builds (crt\recomp_isa_guard.c)
#   lib\recomp_tls.obj                     implicit TLS (thread_local) for the islands mod (crt\recomp_tls.c)
#   build_modules.py, split_units.py, cpu_isa.py, pack.py, LICENSES\, VERSION.txt
#
#   # from an existing official LLVM install:
#   .\tools\toolchain\build-toolchain.ps1 -LlvmDir 'C:\Program Files\LLVM'
#   # downloading the official archive (clang+llvm-<v>-x86_64-pc-windows-msvc.tar.xz from GitHub, 860 MB;
#   # the 264 MB .tar.zst uses a zstd window that Windows' tar.exe cannot open):
#   .\tools\toolchain\build-toolchain.ps1 -Download
#   # plus the compressed archives to measure/publish:
#   .\tools\toolchain\build-toolchain.ps1 -LlvmDir 'C:\Program Files\LLVM' -Compress

[CmdletBinding()]
param(
    [string]$Dest = 'D:\PLARecomp\toolchain-portable\recomp-toolchain',
    [string]$LlvmDir = '',
    [string]$LlvmVersion = '23.1.2',
    [switch]$Download,
    [string]$Cache = 'D:\PLARecomp\toolchain-portable\cache',
    [switch]$Compress
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

function Copy-FromLlvm([string]$root) {
    $bin = Join-Path $root 'bin'
    foreach ($req in 'clang.exe', 'lld-link.exe', 'llvm-objdump.exe') {
        if (-not (Test-Path -LiteralPath (Join-Path $bin $req))) { throw "missing $req in $bin" }
    }
    $ver = & (Join-Path $bin 'clang.exe') --version | Select-Object -First 1
    if ($ver -notmatch [regex]::Escape("clang version $LlvmVersion")) {
        Write-Warning "LLVM in $root is '$ver' (expected $LlvmVersion); objects will not match the official build"
    }
    $resRoot = Get-ChildItem -LiteralPath (Join-Path $root 'lib\clang') -Directory | Select-Object -First 1
    New-Item -ItemType Directory -Force -Path "$Dest\bin" | Out-Null
    # A single clang binary: the executable name selects cl mode.
    Copy-Item -LiteralPath (Join-Path $bin 'clang.exe') -Destination "$Dest\bin\clang-cl.exe" -Force
    Copy-Item -LiteralPath (Join-Path $bin 'lld-link.exe') -Destination "$Dest\bin\lld-link.exe" -Force
    Copy-Item -LiteralPath (Join-Path $bin 'llvm-objdump.exe') -Destination "$Dest\bin\llvm-objdump.exe" -Force
    # Resource headers: freestanding C and the x86 intrinsics. Left out: other architectures,
    # GPU, sanitizers and wrappers (unused by the emitted C, ~12 MB).
    $resDst = "$Dest\lib\clang\$($resRoot.Name)\include"
    New-Item -ItemType Directory -Force -Path $resDst | Out-Null
    $skip = '^(arm|riscv|sifive|hexagon|hvx|htm|altivec|msa|lasx|lsx|larch|vel|wasm|s390|vecintrin|opencl|hlsl|nvptx|amdgpu|cuda|__clang_cuda|__clang_hip|__clang_spirv|spirv|ptrauth|module|cpuid|vadefs|gpuintrin|amdgpuintrin|nvptxintrin|openmp|omp|builtins|zos|prfchwintrin_|cet\.h)'
    Get-ChildItem -LiteralPath (Join-Path $resRoot.FullName 'include') -File -Filter '*.h' |
        Where-Object { $_.Name -notmatch $skip } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $resDst -Force }
    # LLVM license (its binaries and headers are redistributed).
    New-Item -ItemType Directory -Force -Path "$Dest\LICENSES" | Out-Null
    $lic = @(Join-Path $root 'LICENSE.TXT'), (Join-Path $root 'share\doc\LLVM\LICENSE.TXT') |
           Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    $licDst = "$Dest\LICENSES\LLVM-LICENSE.TXT"
    if ($lic) { Copy-Item -LiteralPath $lic -Destination $licDst -Force }
    else {
        # The MSI install and the archive do not ship the text: take it from the same tag.
        $ProgressPreference = 'SilentlyContinue'
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -OutFile $licDst `
            -Uri "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$LlvmVersion/llvm/LICENSE.TXT"
    }
    return $ver
}

if ($Download) {
    # Official GitHub archive (no installer). Only the needed members are extracted with
    # Windows' tar.exe (bsdtar handles .tar.xz).
    $name = "clang+llvm-$LlvmVersion-x86_64-pc-windows-msvc"
    $url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LlvmVersion/$name.tar.xz"
    New-Item -ItemType Directory -Force -Path $Cache | Out-Null
    $arch = Join-Path $Cache "$name.tar.xz"
    if (-not (Test-Path -LiteralPath $arch)) {
        Write-Host "downloading $url"
        $ProgressPreference = 'SilentlyContinue'
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $url -OutFile "$arch.part" -UseBasicParsing
        Move-Item -LiteralPath "$arch.part" -Destination $arch
    }
    $ext = Join-Path $Cache $name
    if (-not (Test-Path -LiteralPath "$ext\bin\llvm-objdump.exe")) {
        & "$env:SystemRoot\System32\tar.exe" -xf $arch -C $Cache "$name/bin/clang.exe" "$name/bin/lld-link.exe" "$name/bin/llvm-objdump.exe" "$name/lib/clang"
        if ($LASTEXITCODE -ne 0) { throw "tar failed ($LASTEXITCODE)" }
    }
    $LlvmDir = $ext
}
if (-not $LlvmDir) { throw 'pass -LlvmDir <official LLVM install> or -Download' }

if (Test-Path -LiteralPath $Dest) { Remove-Item -LiteralPath $Dest -Recurse -Force }
$ver = Copy-FromLlvm $LlvmDir

# Our headers and scripts.
Copy-Item -LiteralPath (Join-Path $here 'include') -Destination "$Dest\include" -Recurse -Force
Copy-Item -LiteralPath (Join-Path $here 'build_modules.py'), (Join-Path $here 'split_units.py'), (Join-Path $here 'cpu_isa.py'), (Join-Path $here 'pack.py') -Destination $Dest -Force

# Our import libraries (lld-link /lib = built-in llvm-lib).
New-Item -ItemType Directory -Force -Path "$Dest\lib" | Out-Null
foreach ($def in Get-ChildItem -LiteralPath (Join-Path $here 'def') -Filter '*.def') {
    $out = Join-Path "$Dest\lib" ($def.BaseName + '.lib')
    & "$Dest\bin\lld-link.exe" /lib /nologo /machine:x64 "/def:$($def.FullName)" "/out:$out"
    if ($LASTEXITCODE -ne 0) { throw "could not generate $out" }
}

# Minimal startup (no /GS: it implements the cookie check itself).
$env:INCLUDE = ''
& "$Dest\bin\clang-cl.exe" /nologo /X "-I$Dest\include" /MD /O2 /GS- /Zl /std:c11 -c `
    (Join-Path $here 'crt\recomp_crt0.c') "/Fo$Dest\lib\recomp_crt0.obj"
if ($LASTEXITCODE -ne 0) { throw 'recomp_crt0.c did not compile' }
# CPU level guards linked into DLLs built with build_modules.py --isa (compiled without -march on purpose).
foreach ($lv in 2, 3) {
    & "$Dest\bin\clang-cl.exe" /nologo /X "-I$Dest\include" /MD /O2 /GS- /Zl /std:c11 "-DRECOMP_ISA_LEVEL=$lv" -c `
        (Join-Path $here 'crt\recomp_isa_guard.c') "/Fo$Dest\lib\recomp_isa_guard_v$lv.obj"
    if ($LASTEXITCODE -ne 0) { throw 'recomp_isa_guard.c did not compile' }
}
& "$Dest\bin\clang-cl.exe" /nologo /X "-I$Dest\include" /MD /O2 /GS- /Zl /std:c11 -c `
    (Join-Path $here 'crt\recomp_tls.c') "/Fo$Dest\lib\recomp_tls.obj"
if ($LASTEXITCODE -ne 0) { throw 'recomp_tls.c did not compile' }

# Self-test: a tiny DLL with an export, the /GS cookie, floating point, a stack probe and one
# import of each kind (vcruntime, UCRT and kernel32), loaded and called from PowerShell.
$test = Join-Path $env:TEMP "recomp-tc-test-$PID"
New-Item -ItemType Directory -Force -Path $test | Out-Null
Set-Content -LiteralPath "$test\t.c" -Encoding ascii -Value @'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <psapi.h>
__declspec(dllexport) double tc_selftest(const char* s, int n) {
    char buf[8192]; volatile double f = 1.5; HMODULE m[4]; DWORD need = 0;
    snprintf(buf, sizeof buf, "%s-%d", s, n); memset(buf + 100, 0, 16);
    K32EnumProcessModules(GetCurrentProcess(), m, sizeof m, &need);
    if (n < 0) { fprintf(stderr, "x"); fflush(stderr); abort(); }
    return f * (double)strlen(buf) + (double)(need != 0) + (calloc(1, 1) != 0);
}
'@
$libs = (Get-ChildItem -LiteralPath "$Dest\lib" -Filter '*.lib').FullName
& "$Dest\bin\clang-cl.exe" /nologo /X "-I$Dest\include" /MD /O2 /std:c11 -c "$test\t.c" "/Fo$test\t.obj"
if ($LASTEXITCODE -ne 0) { throw 'self-test: compile failed' }
& "$Dest\bin\lld-link.exe" /nologo /dll /machine:x64 /nodefaultlib /entry:_DllMainCRTStartup "/out:$test\t.dll" `
    "$test\t.obj" "$Dest\lib\recomp_crt0.obj" @libs
if ($LASTEXITCODE -ne 0) { throw 'self-test: link failed' }
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class RecompTcSelfTest$PID {
  [DllImport(@"$test\t.dll", CallingConvention = CallingConvention.Cdecl)]
  public static extern double tc_selftest([MarshalAs(UnmanagedType.LPStr)] string s, int n);
}
"@
$r = Invoke-Expression "[RecompTcSelfTest$PID]::tc_selftest('abc', 7)"
if ($r -lt 9) { throw "self-test: unexpected result $r" }
Write-Host "self-test OK ($r)"

Set-Content -LiteralPath "$Dest\VERSION.txt" -Encoding ascii -Value @"
recomp-toolchain (portable toolchain of the PC port)
$ver
built: $(Get-Date -Format 'yyyy-MM-dd HH:mm')
usage: python build_modules.py --help
"@

$disk = (Get-ChildItem -LiteralPath $Dest -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("package in {0}: {1:n1} MB on disk" -f $Dest, ($disk / 1MB))

if ($Compress) {
    # Publishing format: .tar.xz with the x86 BCJ filter (pack.py; unpacked with the Python
    # standard library). zip and 7z (if 7-Zip is present) are measured for comparison.
    $parent = Split-Path $Dest -Parent; $leaf = Split-Path $Dest -Leaf
    $res = @()
    $txz = Join-Path $parent "$leaf.tar.xz"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & python -I (Join-Path $here 'pack.py') create $Dest $txz | Out-Host
    $res += [pscustomobject]@{ Format = 'tar.xz BCJ+LZMA2 (publish)'; MB = [math]::Round((Get-Item $txz).Length / 1MB, 1); Sec = [math]::Round($sw.Elapsed.TotalSeconds) }
    $zip = Join-Path $parent "$leaf.zip"
    Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
    $sw.Restart()
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($Dest, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
    $res += [pscustomobject]@{ Format = 'zip (deflate)'; MB = [math]::Round((Get-Item $zip).Length / 1MB, 1); Sec = [math]::Round($sw.Elapsed.TotalSeconds) }
    $7z = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
    if (Test-Path -LiteralPath $7z) {
        $s7 = Join-Path $parent "$leaf.7z"
        Remove-Item -LiteralPath $s7 -ErrorAction SilentlyContinue
        $sw.Restart()
        & $7z a -t7z -mx=9 -mmt=8 -bso0 -bsp0 $s7 $Dest | Out-Null
        $res += [pscustomobject]@{ Format = '7z (LZMA2 -mx9, BCJ2)'; MB = [math]::Round((Get-Item $s7).Length / 1MB, 1); Sec = [math]::Round($sw.Elapsed.TotalSeconds) }
    }
    $res | Format-Table -AutoSize | Out-Host
}
