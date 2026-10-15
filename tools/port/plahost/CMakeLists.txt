# plahost: PLA.exe without an emulator (docs/port/HOST.md).
cmake_minimum_required(VERSION 3.20)
project(plahost CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(PLA_MOD_INCLUDE ${REPO_ROOT}/recomp/mods/include)
if(MSVC)
  add_compile_options(/W4 /wd4324 /utf-8 /EHsc /permissive- /Zi)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX UNICODE _UNICODE)
  # No local checkout path in the binary: __FILE__ is repo-relative and the PDB reference is
  # the file name only (as nvnvk/nnshim).
  get_filename_component(_repo_abs "${REPO_ROOT}" ABSOLUTE)
  file(TO_NATIVE_PATH "${_repo_abs}/" _repo_native)
  add_compile_options("/d1trimfile:${_repo_native}")
  add_link_options("/PDBALTPATH:%_PDB%")
endif()

add_executable(plahost WIN32
  src/main.cpp src/log.cpp src/json.cpp src/memory.cpp src/loader.cpp src/recomp.cpp
  src/kernel.cpp src/sync.cpp src/svc.cpp src/ipc.cpp src/services.cpp src/nvdrv.cpp src/mods.cpp src/tune.cpp src/hle.cpp src/plahost.rc)
target_include_directories(plahost PRIVATE src ${PLA_MOD_INCLUDE})
target_link_libraries(plahost PRIVATE winmm shell32 psapi)
set_target_properties(plahost PROPERTIES OUTPUT_NAME PLA)
if(MSVC)
  # Static CRT: one file beside the game, no redistributable needed.
  set_property(TARGET plahost PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_link_options(plahost PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()
