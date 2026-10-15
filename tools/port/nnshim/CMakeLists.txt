# nnshim: native nn:: layer for the PC port (prototype: nn::hid and nn::fs).
#
#   nnshim_core   static library with the native logic (host types, no guest)
#   nnshim        DLL with the AAPCS64 adapters (mod per recomp/mods/include/pla_mod_api.h)
#                 and the nnshim_get_table() table for other runtimes
#   nnshim_selftest  host-side core test (does not need the game)
#
# Build (VS 2022 Build Tools; from the repo root, in PowerShell):
#   . .\recomp\scripts\env.ps1
#   cmd /c "`"$env:MK8R_VCVARS`" && cmake -S tools\port\nnshim -B build\port\nnshim -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build\port\nnshim"
# SDL2 is optional: -DNNSHIM_WITH_SDL2=ON (needs SDL2Config.cmake in CMAKE_PREFIX_PATH).
cmake_minimum_required(VERSION 3.20)
project(nnshim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(NNSHIM_WITH_SDL2 "SDL2 input backend (GameController)" OFF)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(PLA_MOD_INCLUDE ${REPO_ROOT}/recomp/mods/include)
if(MSVC)
  # Don't embed the local checkout path in the binaries (__FILE__ and the PDB reference).
  get_filename_component(_repo_abs "${REPO_ROOT}" ABSOLUTE)
  file(TO_NATIVE_PATH "${_repo_abs}/" _repo_native)
  add_compile_options("/d1trimfile:${_repo_native}")
  add_link_options("/PDBALTPATH:%_PDB%")
endif()

# Native cores (no guest ABI). audio_core.cpp is added below.
# (hid_core, fs_core, services_core, audio_core... + config and runtime)
file(GLOB NNSHIM_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*_core.cpp)
list(APPEND NNSHIM_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime.cpp)
add_library(nnshim_core STATIC ${NNSHIM_CORE_SOURCES})
target_include_directories(nnshim_core PUBLIC include)
if(MSVC)
  target_compile_options(nnshim_core PRIVATE /W4 /utf-8 /permissive-)
  target_compile_definitions(nnshim_core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(nnshim_core PRIVATE -Wall -Wextra)
endif()
if(WIN32)
  target_link_libraries(nnshim_core PUBLIC xinput ws2_32 user32)
endif()
# Native Opus(opus_core.cpp): host libopus for nn::codec::HardwareOpusDecoder.
# Defaults to the libopus already built by the fork's suyu build; empty = no native Opus.
set(NNSHIM_OPUS_INCLUDE "D:/PLARecomp/mk8-recomp/third_party/suyu/externals/opus/include" CACHE PATH "libopus include dir")
set(NNSHIM_OPUS_LIB "D:/PLARecomp/mk8-recomp/build/suyu/_deps/opus-build/opus.lib" CACHE FILEPATH "static libopus")
if(NNSHIM_OPUS_INCLUDE AND NNSHIM_OPUS_LIB AND EXISTS "${NNSHIM_OPUS_INCLUDE}/opus.h" AND EXISTS "${NNSHIM_OPUS_LIB}")
  target_include_directories(nnshim_core PRIVATE ${NNSHIM_OPUS_INCLUDE})
  target_compile_definitions(nnshim_core PRIVATE NNSHIM_WITH_OPUS=1)
  target_link_libraries(nnshim_core PUBLIC ${NNSHIM_OPUS_LIB})
  message(STATUS "nnshim: native Opus with ${NNSHIM_OPUS_LIB}")
else()
  message(STATUS "nnshim: no native Opus (hwopus stays in the HLE)")
endif()
if(NNSHIM_WITH_SDL2)
  find_package(SDL2 REQUIRED CONFIG)
  target_compile_definitions(nnshim_core PRIVATE NNSHIM_WITH_SDL2=1)
  target_link_libraries(nnshim_core PUBLIC SDL2::SDL2)
endif()

# AAPCS64 adapters: one file per family (abi_*.cpp, audio_abi.cpp...).
file(GLOB NNSHIM_ABI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/abi_*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*_abi.cpp)
list(APPEND NNSHIM_ABI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/table.cpp)
add_library(nnshim SHARED ${NNSHIM_ABI_SOURCES})
target_include_directories(nnshim PRIVATE include src ${PLA_MOD_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nnshim PRIVATE nnshim_core)
if(MSVC)
  target_compile_options(nnshim PRIVATE /W4 /utf-8 /permissive-)
  target_compile_definitions(nnshim PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The test links the adapters directly and calls them with a fake pla_api.
add_executable(nnshim_selftest tests/selftest.cpp ${NNSHIM_ABI_SOURCES})
target_include_directories(nnshim_selftest PRIVATE include src ${PLA_MOD_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nnshim_selftest PRIVATE nnshim_core)
if(MSVC)
  target_compile_options(nnshim_selftest PRIVATE /utf-8)
  target_compile_definitions(nnshim_selftest PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Checks the romfs.bin reader against the extracted folder (docs/port/ROMFS.md).
add_executable(nnshim_romfs_verify tests/romfs_verify.cpp)
target_link_libraries(nnshim_romfs_verify PRIVATE nnshim_core)
if(MSVC)
  target_compile_options(nnshim_romfs_verify PRIVATE /utf-8)
  target_compile_definitions(nnshim_romfs_verify PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
