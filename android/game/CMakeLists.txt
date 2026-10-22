# Android build of Lightning Returns: the game as libmain.so, loaded by SDL3's SDLActivity
# (org.libsdl.app), which calls SDL_main (android/game/lr_android_main.cpp).
#
# Mirrors proj/native/LightningReturns/CMakeLists.txt + generated/rexglue.cmake, except:
#  - shared library instead of an executable; SDL_main entry instead of
#    windowed_app_main_sdl.cpp;
#  - codegen is NOT run (the generated/ sources are used as they are; the host codegen tool
#    is not built for the NDK) - so this never rewrites generated/;
#  - no LR_LOCAL_TEST_HARNESS (test-only code stays out of the APK);
#  - -O2 (not -O3) for the recompiled code.
cmake_minimum_required(VERSION 3.25)
project(lightningreturns_android LANGUAGES C CXX)

if(NOT ANDROID)
  message(FATAL_ERROR "android/game is for the Android NDK toolchain only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

set(LR_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../proj/native/LightningReturns" CACHE PATH
    "Lightning Returns project (src/, generated/)")
get_filename_component(LR_PROJECT_DIR "${LR_PROJECT_DIR}" ABSOLUTE)
set(LR_GENERATED_DIR "${LR_PROJECT_DIR}/generated/default")
# For quick toolchain checks: build only the first N recompiled files (0 = all).
set(LR_RECOMP_LIMIT 0 CACHE STRING "Build only the first N generated recomp files (0 = all)")

find_package(rexglue 0.10.0 CONFIG REQUIRED)

include("${LR_GENERATED_DIR}/sources.cmake")  # GENERATED_SOURCES
if(LR_RECOMP_LIMIT GREATER 0)
  list(SUBLIST GENERATED_SOURCES 0 ${LR_RECOMP_LIMIT} GENERATED_SOURCES)
endif()

# Line tables only (symbolized tombstones / ndk-stack with the unstripped libmain.so); the APK
# gets a stripped copy.
set(LR_RECOMP_OPTIONS -gline-tables-only)

add_library(lightningreturns_recomp OBJECT ${GENERATED_SOURCES})
# One function with ~80k registrar calls, run once at startup: -O2 takes ~8 minutes on it.
set_source_files_properties("${LR_GENERATED_DIR}/lightningreturns_register.cpp"
  PROPERTIES COMPILE_OPTIONS "-O1")
target_include_directories(lightningreturns_recomp PRIVATE
  ${LR_PROJECT_DIR} ${LR_PROJECT_DIR}/src ${LR_GENERATED_DIR})
target_link_libraries(lightningreturns_recomp PRIVATE rex::runtime)
target_compile_options(lightningreturns_recomp PRIVATE ${LR_RECOMP_OPTIONS} -march=armv8-a)
target_precompile_headers(lightningreturns_recomp PRIVATE "${LR_GENERATED_DIR}/lightningreturns_pch.h")

add_library(main SHARED
  ${LR_PROJECT_DIR}/src/main.cpp
  ${LR_PROJECT_DIR}/src/lr_hooks.cpp
  ${REXGLUE_SHARE_DIR}/rex_app.cpp
  ${CMAKE_CURRENT_LIST_DIR}/lr_android_main.cpp
)
target_include_directories(main PRIVATE
  ${LR_PROJECT_DIR} ${LR_PROJECT_DIR}/src ${LR_GENERATED_DIR} ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(main PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")
target_compile_options(main PRIVATE -march=armv8-a -gline-tables-only
  # Android declarations used by the few __ANDROID__ lines in the shared game sources.
  "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/lr_android.h")
target_link_libraries(main PRIVATE lightningreturns_recomp rex::runtime android log)
target_link_options(main PRIVATE -Wl,--as-needed)
