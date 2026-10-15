# CMake toolchain to compile the game's code as AArch64 objects (Switch)
# and link a "uking-like" shared ELF (like zeldaret/botw) that viking
# can compare symbol by symbol against data/main.elf.
#
# Choosing the clang (in this order of priority):
#   1. -DPLA_CLANG=<version>|<path to a clang directory>
#   2. environment variable PLA_CLANG (same format)
#   3. default 9.0.0
# A version "X.Y.Z" resolves to toolchain/cache/clang-X.Y.Z (see fetch_clang.py).
#
# Other options (all -D...):
#   PLA_TRIPLE      aarch64-none-elf (default) | aarch64-linux-elf (what botw uses)
#   PLA_OPT         -O3 (default) | -O2 | -Os
#   PLA_EXTRA_FLAGS additional flags (e.g. "-fno-omit-frame-pointer")

set(_pla_default_clang "9.0.0")
if (DEFINED PLA_CLANG)
    set(_pla_clang "${PLA_CLANG}")
elseif (DEFINED ENV{PLA_CLANG})
    set(_pla_clang "$ENV{PLA_CLANG}")
else()
    set(_pla_clang "${_pla_default_clang}")
endif()
if (_pla_clang MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    set(_pla_clang_dir "${CMAKE_CURRENT_LIST_DIR}/cache/clang-${_pla_clang}")
    set(_pla_clang_ver "${_pla_clang}")
else()
    set(_pla_clang_dir "${_pla_clang}")
    file(GLOB _pla_res "${_pla_clang_dir}/lib/clang/*")
    list(GET _pla_res 0 _pla_res)
    get_filename_component(_pla_clang_ver "${_pla_res}" NAME)
endif()
# Stored in the cache so that CMakeLists.txt and try_compile see them.
set(PLA_CLANG "${_pla_clang}" CACHE STRING "Version or path of the candidate clang")
set(PLA_CLANG_DIR "${_pla_clang_dir}" CACHE PATH "Directory of the clang in use" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PLA_CLANG PLA_TRIPLE PLA_OPT)

if (CMAKE_HOST_WIN32)
    set(_exe ".exe")
else()
    set(_exe "")
endif()
if (NOT EXISTS "${_pla_clang_dir}/bin/clang${_exe}")
    message(FATAL_ERROR "clang not found in ${_pla_clang_dir}. "
        "Run: python toolchain/fetch_clang.py ${_pla_clang}")
endif()

if (NOT DEFINED PLA_TRIPLE)
    set(PLA_TRIPLE aarch64-none-elf)
endif()
if (NOT DEFINED PLA_OPT)
    set(PLA_OPT -O3)
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_EXECUTABLE_SUFFIX "")
# A test executable for Switch cannot be linked: test with static libraries.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER "${_pla_clang_dir}/bin/clang${_exe}")
set(CMAKE_CXX_COMPILER "${_pla_clang_dir}/bin/clang++${_exe}")
set(CMAKE_C_COMPILER_TARGET ${PLA_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${PLA_TRIPLE})
set(CMAKE_LINKER "${_pla_clang_dir}/bin/ld.lld${_exe}")
set(CMAKE_OBJDUMP "${_pla_clang_dir}/bin/llvm-objdump${_exe}")

# Optimization: always applied, whatever CMAKE_BUILD_TYPE is (what matters
# is the codegen). The per-configuration flags are emptied so -O0/-O2 are not mixed.
foreach (_cfg RELEASE RELWITHDEBINFO MINSIZEREL DEBUG)
    set(CMAKE_C_FLAGS_${_cfg}_INIT "")
    set(CMAKE_CXX_FLAGS_${_cfg}_INIT "")
endforeach()
add_compile_options(${PLA_OPT} -g)
set(CMAKE_C_FLAGS_INIT "${PLA_EXTRA_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${PLA_EXTRA_FLAGS}")

# --- Target options (Switch's Cortex-A57) ---
add_compile_options(-mcpu=cortex-a57+fp+simd+crypto+crc)
add_compile_options(-mno-implicit-float)
add_compile_options(-fPIC)
add_compile_options(-ffunction-sections -fdata-sections)
add_compile_options(-fno-exceptions)
add_compile_options(-fstandalone-debug)

# --- Headers: libc++ of the same LLVM version + musl (botw-lib-musl) ---
# ALL system paths are disabled and the order is fixed by hand:
# libc++ -> clang internal headers -> musl (the same order the Linux
# driver uses with -stdlib=libc++), so that no host headers sneak in.
set(_pla_resource_inc "${_pla_clang_dir}/lib/clang/${_pla_clang_ver}/include")
add_compile_options(-nostdinc)
add_compile_options("SHELL:-isystem ${_pla_clang_dir}/libcxx-include")
add_compile_options("SHELL:-isystem ${_pla_resource_inc}")
add_compile_options("SHELL:-isystem ${CMAKE_CURRENT_LIST_DIR}/musl/include")

add_compile_definitions(SWITCH NNSDK MATCHING_HACK_NX_CLANG)

# --- Linking: ld.lld directly (not the driver, which changes from version to version) ---
# Shared ELF like botw's: -shared + .plt/.rela.dyn/DT_HASH, which viking
# needs (build_glob_data_table and get_plt_functions).
set(PLA_LINK_FLAGS "-shared -Bsymbolic-functions --hash-style=sysv -z notext --no-gc-sections"
    CACHE STRING "Flags passed directly to ld.lld")
set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_LINKER> ${PLA_LINK_FLAGS} <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}")
