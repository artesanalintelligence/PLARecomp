cmake_minimum_required(VERSION 3.20)
# Configure with:
#   cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/ToolchainNX64.cmake [-DPLA_CLANG=9.0.0]
project(pla CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  add_compile_options(-fcolor-diagnostics -Wno-unused-command-line-argument)
endif()

# All of src/**/*.cpp except src/_smoke (the smoke test has its own target).
# The decomp sources in src/ are local only (gitignored, see docs/PUBLISHING.md).
file(GLOB_RECURSE PLA_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cpp)
list(FILTER PLA_SOURCES EXCLUDE REGEX "/src/_smoke/")

# The target name must match build_target in tools/config.toml:
# viking compares build/pla against data/main.elf.
if (PLA_SOURCES)
  add_executable(pla ${PLA_SOURCES})
  target_include_directories(pla PRIVATE src include lib)
  # PLA_NO_HIDDEN_EXTERNS: see include/types.h (tools/decomp/verify.py compiles without it).
  target_compile_definitions(pla PRIVATE NON_MATCHING PLA_NO_HIDDEN_EXTERNS)
  target_compile_options(pla PRIVATE -fno-rtti -fno-strict-aliasing)
  target_compile_options(pla PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-invalid-offsetof)
else()
  message(STATUS "src/ has no .cpp files yet: the 'pla' target is not created")
endif()

# Toolchain smoke test (does not need the game).
file(GLOB PLA_SMOKE_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/_smoke/*.cpp)
if (PLA_SMOKE_SOURCES)
  add_executable(pla_smoke EXCLUDE_FROM_ALL ${PLA_SMOKE_SOURCES})
  target_compile_options(pla_smoke PRIVATE -fno-rtti)
endif()
