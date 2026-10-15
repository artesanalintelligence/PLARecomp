@echo off
rem Builds nvnshader into build\port\nvnshader\cmake (Release, Ninja, MSVC x64).
rem Usage: tools\port\nvnshader\build.cmd [jobs]
setlocal
set JOBS=%1
if "%JOBS%"=="" set JOBS=8
set REPO=%~dp0..\..\..
set BLD=%REPO%\build\port\nvnshader\cmake
set PATH=C:\Program Files\CMake\bin;%PATH%
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%REPO%\tools\port\nvnshader" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNVNSHADER_WITH_SPIRV_OPT=ON || exit /b 1
rem Spanish-locale cl.exe writes /showIncludes in the OEM code page ("inclusi\xA2n") and CMake stores the
rem prefix in UTF-8: Ninja misses header dependencies (PERF note in REQUESTS.md).
rem Rewrite the prefix with the OEM byte so .h changes rebuild their dependents.
python -c "import pathlib,sys;p=pathlib.Path(sys.argv[1]);b=p.read_bytes();n=b.replace('inclusi\u00f3n'.encode('utf-8'),b'inclusi\xa2n');(n!=b) and p.write_bytes(n)" "%BLD%\CMakeFiles\rules.ninja"
cmake --build "%BLD%" --parallel %JOBS%
