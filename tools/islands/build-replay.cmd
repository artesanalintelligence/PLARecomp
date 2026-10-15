@call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d D:\PLARecomp\fps\fps4\src
"C:\Program Files\LLVM\bin\clang-cl.exe" /nologo /O2 /MD /EHsc /std:c++20 -D_CRT_SECURE_NO_WARNINGS -msse4.1 -mfma -mavx -fno-strict-aliasing isl_replay.cpp /Fe:..\build\isl_replay.exe /Fo:..\build\isl_replay.obj /link /NOLOGO kernel32.lib
echo exit %ERRORLEVEL%
