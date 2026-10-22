# Lightning Returns port: rebuilds ReXGlue's GPU plugin (rexgpu-xenos) from the v0.10.0
# sources in sdk/, linked against the installed ReXGlue SDK's rexruntime, so plugin-side
# changes (texture replacement) ship without rebuilding the SDK.
cmake_minimum_required(VERSION 3.25)
project(lr_rexgpu_xenos C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT WIN32)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

find_package(rexglue 0.10.0 CONFIG REQUIRED)

# Same global settings the SDK build uses (they affect code generation, not layout).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
  add_compile_options(-march=x86-64-v2)  # as the SDK presets
endif()
add_compile_options(-fno-strict-aliasing -ffp-model=strict -fno-char8_t
                    $<$<CONFIG:RELEASE>:-O3> $<$<CONFIG:RELEASE>:-DNDEBUG>)
if(WIN32)
  add_compile_definitions(REX_PLATFORM_WINDOWS=1)
  set(REXGLUE_USE_D3D12 ON)
else()
  add_compile_definitions(REX_PLATFORM_LINUX=1)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    add_compile_options(-mcmodel=large)
  endif()
  set(REXGLUE_USE_D3D12 OFF)
endif()
add_compile_definitions(REXGLUE_BUILD_CONFIG="$<CONFIG>")

set(REXGLUE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
include(${REXGLUE_ROOT}/rex_pch.cmake)
# The SDK's own headers first: the plugin includes private graphics headers that the
# installed package does not ship.
include_directories(BEFORE ${REXGLUE_ROOT}/include)

# Names the SDK's graphics CMakeLists links against, mapped onto the installed package.
add_library(rexruntime INTERFACE)
target_link_libraries(rexruntime INTERFACE rex::runtime rex::renderdoc)
if(NOT TARGET xxHash::xxhash)
  add_library(xxHash::xxhash ALIAS rex::xxhash)
endif()
if(REXGLUE_USE_VULKAN)
  foreach(lib SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler)
    if(NOT TARGET ${lib})
      add_library(${lib} INTERFACE)
      target_link_libraries(${lib} INTERFACE rex::${lib})
    endif()
  endforeach()
  # glslang's SPIRV builder headers (commit f4f1d8a, matching the SDK's static libraries).
  set(LR_GLSLANG_SOURCE "" CACHE PATH "glslang source tree (Vulkan builds only)")
  target_include_directories(SPIRV INTERFACE ${LR_GLSLANG_SOURCE})
  if(NOT TARGET glslang::SPIRV)
    add_library(glslang::SPIRV ALIAS SPIRV)
  endif()
endif()

add_subdirectory(${REXGLUE_ROOT}/graphics ${CMAKE_BINARY_DIR}/graphics)
target_compile_options(rexgpu-xenos PRIVATE -Wno-unused-parameter -Wno-unknown-pragmas)
if(WIN32)
  # Symbol map for profiling (LR_Profile.ps1 copies it next to the dumps).
  target_link_options(rexgpu-xenos PRIVATE -Xlinker /MAP:${CMAKE_BINARY_DIR}/rexgpu-xenos.map)
endif()
