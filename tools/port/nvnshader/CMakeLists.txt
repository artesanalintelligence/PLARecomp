# SPDX-License-Identifier: GPL-3.0-or-later
#
# nvnshader: suyu's shader_recompiler (Maxwell -> IR -> SPIR-V) isolated from video_core/core,
# with an Environment for NVN shaders and an offline tool.
#
# Dependencies (read-only, by default from the suyu fork's CPM cache):
#   fmt 12.1 (sources), boost 1.90 (headers only: container, intrusive...), sirit 1.0.5
#   (prebuilt, MSVC x64), ankerl::unordered_dense; Vulkan SDK (SPIRV-Tools, for validation).
cmake_minimum_required(VERSION 3.22)
project(nvnshader CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if (MSVC)
    add_compile_options(/utf-8 /EHsc)
endif()

set(NVNSHADER_DEPS_ROOT "D:/PLARecomp/mk8-recomp/third_party/suyu/.cache/cpm" CACHE PATH
    "CPM cache of the suyu fork (fmt, boost, sirit, unordered_dense)")
set(NVNSHADER_FMT_DIR "${NVNSHADER_DEPS_ROOT}/fmt/12.1.0" CACHE PATH "")
set(NVNSHADER_BOOST_DIR "${NVNSHADER_DEPS_ROOT}/boost/boost-1.90.0" CACHE PATH "")
set(NVNSHADER_SIRIT_DIR "${NVNSHADER_DEPS_ROOT}/sirit/1.0.5-windows-amd64" CACHE PATH "")
set(NVNSHADER_UDENSE_DIR "${NVNSHADER_DEPS_ROOT}/unordered_dense/7b55cab841" CACHE PATH "")
if (DEFINED ENV{VULKAN_SDK})
    set(_vk_default "$ENV{VULKAN_SDK}")
else()
    set(_vk_default "C:/VulkanSDK/1.4.328.1")
endif()
set(NVNSHADER_VULKAN_SDK "${_vk_default}" CACHE PATH "Vulkan SDK (SPIRV-Tools)")
option(NVNSHADER_BUILD_TOOL "Build nvnshader_tool (requires the Vulkan SDK)" ON)

# --- fmt ---
add_library(nvnshader_fmt STATIC
    ${NVNSHADER_FMT_DIR}/src/format.cc
    ${NVNSHADER_FMT_DIR}/src/os.cc)
target_include_directories(nvnshader_fmt PUBLIC ${NVNSHADER_FMT_DIR}/include)

# --- boost (headers only; modular libs/*/include layout) ---
file(GLOB _boost_incs LIST_DIRECTORIES true "${NVNSHADER_BOOST_DIR}/libs/*/include")
file(GLOB _boost_incs2 LIST_DIRECTORIES true "${NVNSHADER_BOOST_DIR}/libs/numeric/*/include")
add_library(nvnshader_boost INTERFACE)
target_include_directories(nvnshader_boost SYSTEM INTERFACE ${_boost_incs} ${_boost_incs2})
target_compile_definitions(nvnshader_boost INTERFACE BOOST_ALL_NO_LIB)

# --- sirit (prebuilt) ---
add_library(nvnshader_sirit STATIC IMPORTED)
set_target_properties(nvnshader_sirit PROPERTIES
    IMPORTED_LOCATION "${NVNSHADER_SIRIT_DIR}/lib/sirit-static.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${NVNSHADER_SIRIT_DIR}/include")

# --- shader_recompiler (copied from the fork, see third_party/shader_recompiler/PROVENANCE.txt) ---
set(SR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/shader_recompiler/src)
include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)
add_library(nvnshader_recompiler STATIC ${SR_SOURCES} compat/common_compat.cpp)
target_include_directories(nvnshader_recompiler PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/compat
    ${SR_ROOT}
    ${NVNSHADER_UDENSE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../nvnvk/include)
target_link_libraries(nvnshader_recompiler PUBLIC nvnshader_fmt nvnshader_boost nvnshader_sirit)
if (MSVC)
    target_compile_options(nvnshader_recompiler PRIVATE /bigobj /MP /W3 /wd4244 /wd4267 /wd4100
        /utf-8 /permissive- /Zc:preprocessor /EHsc)
    target_compile_definitions(nvnshader_recompiler PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# --- nvnshader (own API) ---
add_library(nvnshader STATIC
    src/nvnshader.cpp
    src/nvn_environment.cpp
    src/spirv_opt.cpp)
option(NVNSHADER_WITH_SPIRV_OPT "Optimize SPIR-V with SPIRV-Tools (static, +MB; barely helps: see SHADERS.md §12)" OFF)
if (NVNSHADER_WITH_SPIRV_OPT AND EXISTS "${NVNSHADER_VULKAN_SDK}/Lib/SPIRV-Tools-opt.lib")
    # Static (SDK Release /MD): the optimizer C API is not exported from the DLL.
    target_compile_definitions(nvnshader PRIVATE NVNSHADER_WITH_SPIRV_OPT=1)
    target_include_directories(nvnshader PUBLIC ${NVNSHADER_VULKAN_SDK}/Include)
    target_link_libraries(nvnshader PUBLIC ${NVNSHADER_VULKAN_SDK}/Lib/SPIRV-Tools-opt.lib
        ${NVNSHADER_VULKAN_SDK}/Lib/SPIRV-Tools.lib)
else()
    target_compile_definitions(nvnshader PRIVATE NVNSHADER_WITH_SPIRV_OPT=0)
endif()
target_include_directories(nvnshader PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../nvnvk/include)
target_link_libraries(nvnshader PUBLIC nvnshader_recompiler)
if (MSVC)
    target_compile_options(nvnshader PRIVATE /W4 /utf-8 /permissive- /EHsc /wd4100)
endif()

if (NVNSHADER_BUILD_TOOL)
    add_executable(nvnshader_tool tools/nvnshader_tool.cpp)
    target_include_directories(nvnshader_tool PRIVATE ${NVNSHADER_VULKAN_SDK}/Include)
    target_link_libraries(nvnshader_tool PRIVATE nvnshader)
    if (NOT NVNSHADER_WITH_SPIRV_OPT)
        target_link_libraries(nvnshader_tool PRIVATE ${NVNSHADER_VULKAN_SDK}/Lib/SPIRV-Tools.lib)
    endif()
    if (MSVC)
        target_compile_options(nvnshader_tool PRIVATE /W4 /utf-8 /EHsc)
    endif()
endif()

if (NVNSHADER_BUILD_TOOL)
    add_executable(nvnshader_vkcheck tools/nvnshader_vkcheck.cpp)
    target_include_directories(nvnshader_vkcheck PRIVATE ${NVNSHADER_VULKAN_SDK}/Include)
    target_link_libraries(nvnshader_vkcheck PRIVATE nvnshader ${NVNSHADER_VULKAN_SDK}/Lib/vulkan-1.lib)
    if (MSVC)
        target_compile_options(nvnshader_vkcheck PRIVATE /W4 /utf-8 /EHsc)
    endif()
endif()
