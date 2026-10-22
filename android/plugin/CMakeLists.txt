# Android build of the Lightning Returns GPU plugin (rexgpu-xenos, Vulkan only).
# Wraps kit/plugin unchanged: kit/plugin puts its own copy of the SDK headers
# (kit/plugin/sdk/include) first, which lacks the Android fixes of the SDK headers
# (patches/rexglue-sdk-android.patch). LR_ANDROID_HEADER_OVERLAY holds just those patched,
# non-graphics headers and is put in front of everything for the plugin target.
cmake_minimum_required(VERSION 3.25)
project(lr_rexgpu_xenos_android C CXX)

if(NOT ANDROID)
  message(FATAL_ERROR "android/plugin is for the Android NDK toolchain only")
endif()
set(LR_KIT_PLUGIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../../kit/plugin" CACHE PATH "kit/plugin")
set(LR_ANDROID_HEADER_OVERLAY "" CACHE PATH "Android-patched SDK headers")

add_subdirectory(${LR_KIT_PLUGIN_DIR} kit-plugin)

if(LR_ANDROID_HEADER_OVERLAY)
  target_include_directories(rexgpu-xenos BEFORE PRIVATE ${LR_ANDROID_HEADER_OVERLAY})
endif()
# The runtime loads the plugin from the APK's native library directory; keep the soname plain.
set_target_properties(rexgpu-xenos PROPERTIES OUTPUT_NAME rexgpu-xenos)
