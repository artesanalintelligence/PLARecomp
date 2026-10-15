@echo off
rem Builds nnshim with MSVC and runs the selftest (from the repo root). The devkitPro/msys2 cmake
rem on PATH breaks the cache (CMAKE_COMMAND with /home/... paths): so Kitware's cmake and cl are forced.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\CMake\bin;%PATH%"
cmake -S tools\port\nnshim -B build\port\nnshim -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl || exit /b 1
cmake --build build\port\nnshim || exit /b 1
build\port\nnshim\nnshim_selftest.exe
