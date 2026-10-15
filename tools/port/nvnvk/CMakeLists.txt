cmake_minimum_required(VERSION 3.20)
project(nvnvk CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(PLA_MOD_INCLUDE ${REPO_ROOT}/recomp/mods/include)
if(MSVC)
  add_compile_options(/W4 /utf-8 /EHsc /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
  # Don't embed the local checkout path in the binaries: __FILE__ (asserts, logs) becomes repo-relative and
  # the PDB reference keeps only the file name.
  get_filename_component(_repo_abs "${REPO_ROOT}" ABSOLUTE)
  file(TO_NATIVE_PATH "${_repo_abs}/" _repo_native)
  add_compile_options("/d1trimfile:${_repo_native}")
  add_link_options("/PDBALTPATH:%_PDB%")
endif()

# NVN tracer (phase 2)
add_library(nvntrace SHARED trace/nvntrace.cpp)
target_include_directories(nvntrace PRIVATE ${PLA_MOD_INCLUDE} trace generated)

# NVN -> Vulkan backend (phase 4)
find_package(Vulkan REQUIRED)
# In-game overlay menu (overlay*.cpp): Dear ImGui (third_party/imgui, MIT) and Lato fonts (third_party/fonts/lato,
# SIL OFL 1.1) embedded as resources.
set(NVNVK_IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(NVNVK_LATO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/lato)
configure_file(src/nvnvk.rc.in ${CMAKE_CURRENT_BINARY_DIR}/nvnvk.rc @ONLY)
add_library(nvnvk SHARED
  src/mod_main.cpp src/objects.cpp src/record.cpp src/queue.cpp src/exec.cpp src/vkctx.cpp
  src/textures.cpp src/formats.cpp src/pipeline.cpp src/blocklinear.cpp src/capture.cpp src/hostmem.cpp
  src/overlay.cpp src/overlay_ui.cpp src/options.cpp
  ${NVNVK_IMGUI}/imgui.cpp ${NVNVK_IMGUI}/imgui_draw.cpp ${NVNVK_IMGUI}/imgui_tables.cpp ${NVNVK_IMGUI}/imgui_widgets.cpp
  ${NVNVK_IMGUI}/backends/imgui_impl_win32.cpp ${NVNVK_IMGUI}/backends/imgui_impl_vulkan.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/nvnvk.rc)
target_include_directories(nvnvk PRIVATE ${PLA_MOD_INCLUDE} src generated include ${NVNVK_IMGUI})
target_compile_definitions(nvnvk PRIVATE IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/nvnvk.rc PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nvnvk PRIVATE Vulkan::Vulkan)
if(MSVC)
  target_compile_options(nvnvk PRIVATE /Zi)
  target_link_options(nvnvk PRIVATE /DEBUG /OPT:REF /OPT:ICF /MAP)
endif()

# V2-SHADERS shader translator (static library, built in this build tree)
option(NVNVK_WITH_NVNSHADER "Link tools/port/nvnshader" ON)
if(NVNVK_WITH_NVNSHADER AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../nvnshader/CMakeLists.txt)
  set(NVNSHADER_BUILD_TOOL OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../nvnshader ${CMAKE_BINARY_DIR}/nvnshader)
  target_link_libraries(nvnvk PRIVATE nvnshader)
  target_compile_definitions(nvnvk PRIVATE NVNVK_WITH_NVNSHADER=1)
endif()

# Tests that run without the game
add_executable(nvnvk_blocklinear_test tests/blocklinear_test.cpp src/blocklinear.cpp)
target_include_directories(nvnvk_blocklinear_test PRIVATE src)

# Offline player for .nvkc captures (loads nvnvk.dll with a fake pla_api)
add_executable(nvnvk_replay tools/replay/nvnvk_replay.cpp tools/replay/second_presenter.cpp)
target_include_directories(nvnvk_replay PRIVATE ${PLA_MOD_INCLUDE})
target_link_libraries(nvnvk_replay PRIVATE Vulkan::Vulkan)
add_dependencies(nvnvk_replay nvnvk)
