# lightningreturns - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(lightningreturns LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(LIGHTNINGRETURNS_SOURCES
    src/main.cpp
    src/lr_hooks.cpp
)

if(WIN32)
    add_executable(lightningreturns WIN32 ${LIGHTNINGRETURNS_SOURCES})
else()
    add_executable(lightningreturns ${LIGHTNINGRETURNS_SOURCES})
endif()

# The window / exe icon: made at build time from the game's own title image (the builder writes
# src/lightningreturns.ico and .rc); never part of the repository.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/lightningreturns.rc")
    target_sources(lightningreturns PRIVATE src/lightningreturns.rc)
endif()

rexglue_setup_target(lightningreturns)

# The recompiled game code is built for the CPU of the PC that builds it (the game is built on
# the PC that plays it): byte-swapped loads become MOVBE, the 3953 PowerPC multiply-adds
# (std::fma) single FMA instructions instead of library calls, vector code AVX encodings.
# -ffp-contract=off: no other multiply / add pairs are fused, so float results stay exactly as
# with the SSE4.1 build. LR_NATIVE_CPU=OFF builds an exe that runs on any x86-64 PC with SSE4.1.
option(LR_NATIVE_CPU "Build the recompiled code for this PC's CPU" ON)
if(LR_NATIVE_CPU AND NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(lightningreturns_recomp PRIVATE -march=native -ffp-contract=off)
    target_compile_options(lightningreturns PRIVATE -march=native -ffp-contract=off)
endif()
