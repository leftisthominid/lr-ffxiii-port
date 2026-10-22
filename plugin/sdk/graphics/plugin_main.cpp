/**
 * @file        graphics/plugin_main.cpp
 * @brief       rexgpu-xenos plugin entry points
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <string_view>

#include <rex/graphics/lr_frame_stats.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

// Lightning Returns port: Settings > Resolution applied while the game runs. The renderer
// thread switches to this draw resolution scale (1-3) at the next frame boundary.
extern "C" REX_GPU_PLUGIN_EXPORT void lr_set_resolution_scale(int32_t scale) {
  rex::graphics::lr::requested_resolution_scale().store(scale);
}

// The scale the renderer uses now (after a switch it could not make, still the old one); 0
// before the renderer is set up.
extern "C" REX_GPU_PLUGIN_EXPORT int32_t lr_get_resolution_scale(void) {
  return rex::graphics::lr::active_resolution_scale().load();
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("rexgpu-xenos: host requested ABI {}, plugin is ABI {}", abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-xenos: invalid GpuCreateInfo");
    return nullptr;
  }

  std::string_view backend = info->backend ? info->backend : "any";
#if REX_HAS_D3D12
  if (backend == "any" || backend == "d3d12") {
    return new rex::graphics::d3d12::D3D12GraphicsSystem();
  }
#endif
#if REX_HAS_VULKAN
  if (backend == "any" || backend == "vulkan") {
    return new rex::graphics::vulkan::VulkanGraphicsSystem();
  }
#endif
  REXLOG_ERROR("rexgpu-xenos: requested backend '{}' is not compiled into this plugin", backend);
  return nullptr;
}
