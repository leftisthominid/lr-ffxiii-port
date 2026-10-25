/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/lr_frame_stats.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/replacement.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/system/xmemory.h>

#include <xxhash.h>

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_render_to_texture, 24, "GPU",
                     "Texture cache memory limit for render-to-texture (MB)")
    .range(1, 256)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_soft, 384, "GPU",
                     "Soft texture cache memory limit (MB)")
    .range(64, 4096)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_hard, 768, "GPU",
                     "Hard texture cache memory limit (MB)")
    .range(128, 8192)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(texture_cache_memory_limit_soft_lifetime, 30, "GPU",
                     "Soft texture cache memory limit lifetime (seconds)")
    .range(1, 3600);

REXCVAR_DEFINE_BOOL(gpu_3d_to_2d_texture, true, "GPU",
                    "Sample problematic 3D textures through 2D-compatible wrappers")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(anisotropic_override, 3, "GPU",
                     "Forces anisotropic filtering for eligible textures.\n"
                     "Higher values keep textures sharper at oblique angles, but increase texture "
                     "sampling cost.\n"
                     " -1 = No override\n"
                     "  0 = Disable anisotropic filtering\n"
                     "  1 = Force 1x anisotropic filtering\n"
                     "  2 = Force 2x anisotropic filtering\n"
                     "  3 = Force 4x anisotropic filtering\n"
                     "  4 = Force 8x anisotropic filtering\n"
                     "  5 = Force 16x anisotropic filtering")
    .range(-1, 5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(draw_resolution_scale_x, 1, "GPU", "Draw resolution scale X (1 = no scaling)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(draw_resolution_scale_y, 1, "GPU", "Draw resolution scale Y (1 = no scaling)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(resolution_scale, 1, "GPU",
                     "Draw resolution scale for both X and Y axes (same as setting "
                     "draw_resolution_scale_x and draw_resolution_scale_y)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(pre_mask_resolve_l2_block, true, "GPU",
                    "Pre-mask scaled resolve L2 blocks to the write range before iterating");

// DEFINE_int32(
//     draw_resolution_scale_x, 1,
//     "Integer pixel width scale used for scaling the rendering resolution "
//     "opaquely to the game.\n"
//     "1, 2 and 3 may be supported, but support of anything above 1 depends on "
//     "the device properties, such as whether it supports sparse binding / tiled "
//     "resources, the number of virtual address bits per resource, and other "
//     "factors.\n"
//     "Various effects and parts of game rendering pipelines may work "
//     "incorrectly as pixels become ambiguous from the game's perspective and "
//     "because half-pixel offset (which normally doesn't affect coverage when "
//     "MSAA isn't used) becomes full-pixel.",
//     "GPU");
// DEFINE_int32(
//     draw_resolution_scale_y, 1,
//     "Integer pixel width scale used for scaling the rendering resolution "
//     "opaquely to the game.\n"
//     "See draw_resolution_scale_x for more information.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_soft, 384,
//     "Maximum host texture memory usage (in megabytes) above which old textures "
//     "will be destroyed.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_soft_lifetime, 30,
//     "Seconds a texture should be unused to be considered old enough to be "
//     "deleted if texture memory usage exceeds texture_cache_memory_limit_soft.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_hard, 768,
//     "Maximum host texture memory usage (in megabytes) above which textures "
//     "will be destroyed as soon as possible.",
//     "GPU");
// DEFINE_uint32(
//     texture_cache_memory_limit_render_to_texture, 24,
//     "Part of the host texture memory budget (in megabytes) that will be scaled "
//     "by the current drawing resolution scale.\n"
//     "If texture_cache_memory_limit_soft, for instance, is 384, and this is 24, "
//     "it will be assumed that the game will be using roughly 24 MB of "
//     "render-to-texture (resolve) targets and 384 - 24 = 360 MB of regular "
//     "textures - so with 2x2 resolution scaling, the soft limit will be 360 + "
//     "96 MB, and with 3x3, it will be 360 + 216 MB.",
//     "GPU");

namespace rex::graphics {

const TextureCache::LoadShaderInfo TextureCache::load_shader_info_[kLoadShaderCount] = {
    // k8bpb
    {3, 4, 1, 4},
    // k16bpb
    {4, 4, 2, 4},
    // k32bpb
    {4, 4, 4, 3},
    // k64bpb
    {4, 4, 8, 2},
    // k128bpb
    {4, 4, 16, 1},
    // kR5G5B5A1ToB5G5R5A1
    {4, 4, 2, 4},
    // kR5G6B5ToB5G6R5
    {4, 4, 2, 4},
    // kR5G6B5ToRGBA8
    {4, 4, 4, 4},
    // kR5G5B6ToB5G6R5WithRBGASwizzle
    {4, 4, 2, 4},
    // kRGBA4ToBGRA4
    {4, 4, 2, 4},
    // kRGBA4ToARGB4
    {4, 4, 2, 4},
    // kRGBA4ToRGBA8
    {4, 4, 4, 4},
    // kGBGR8ToGRGB8
    {4, 4, 4, 3},
    // kGBGR8ToRGB8
    {4, 4, 8, 3},
    // kBGRG8ToRGBG8
    {4, 4, 4, 3},
    // kBGRG8ToRGB8
    {4, 4, 8, 3},
    // kR10G11B11ToRGBA16
    {4, 4, 8, 3},
    // kR10G11B11ToRGBA16SNorm
    {4, 4, 8, 3},
    // kR11G11B10ToRGBA16
    {4, 4, 8, 3},
    // kR11G11B10ToRGBA16SNorm
    {4, 4, 8, 3},
    // kR16UNormToFloat
    {4, 4, 2, 4},
    // kR16SNormToFloat
    {4, 4, 2, 4},
    // kRG16UNormToFloat
    {4, 4, 4, 3},
    // kRG16SNormToFloat
    {4, 4, 4, 3},
    // kRGBA16UNormToFloat
    {4, 4, 8, 2},
    // kRGBA16SNormToFloat
    {4, 4, 8, 2},
    // kDXT1ToRGBA8
    {4, 4, 4, 2},
    // kDXT3ToRGBA8
    {4, 4, 4, 1},
    // kDXT5ToRGBA8
    {4, 4, 4, 1},
    // kDXNToRG8
    {4, 4, 2, 1},
    // kDXT3A
    {4, 4, 1, 2},
    // kDXT3AAs1111ToBGRA4
    {4, 4, 2, 2},
    // kDXT3AAs1111ToARGB4
    {4, 4, 2, 2},
    // kDXT5AToR8
    {4, 4, 1, 2},
    // kCTX1
    {4, 4, 2, 2},
    // kDepthUnorm
    {4, 4, 4, 3},
    // kDepthFloat
    {4, 4, 4, 3},
};

// Lightning Returns: the replacement worker (see LrReplaceQueue in cache.h).
struct TextureCache::LrReplaceQueue {
  struct Job {
    TextureKey key;
    uint64_t raw_hash = 0;
    const uint8_t* data = nullptr;
    uint32_t size = 0, width = 0, height = 0, blocks_w = 0, blocks_h = 0, bytes_per_block = 0,
             pitch_blocks = 0;
    xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
    bool tiled = false;
    bool recheck = false;
    // The result for these raw bytes is already known; only the DDS needs reading.
    bool known = false;
    LrRawResult result = {};
    // Filled by the worker.
    bool stale = false;  // the game rewrote the memory while it was being read
    bool sig_tried = false;
    float diff = 0.0f;
    const texture_replacement::Entry* entry = nullptr;
    uint64_t entry_hash = 0;
    std::shared_ptr<texture_replacement::Image> image;
    std::string error;
    // Font atlas (see FontComposite in replacement.h): the share of blocks equal to the best
    // original, and whether the image is the replacement with the changed blocks rebuilt.
    float font_equal = -1.0f;
    int font_layout = 0;  // 1: other tiling, 2: 16-bit bytes swapped
    uint32_t font_dirty = 0, font_changed = 0, font_unfilled = 0;
    bool font_full = false;  // font_equal is over every glyph block (else over a sample)
    // 8-byte groups still holding the 0xCC fill of memory the game has not loaded yet.
    uint32_t fill_groups = 0;
    bool composite = false;
    std::string font_desc;  // which atlas tiles a font window shows
  };

  // Originals of the font atlases and recently read replacement images (worker thread only).
  struct FontSource {
    uint32_t blocks_w = 0, blocks_h = 0, tiles_w = 0, tiles_h = 0;
    std::vector<uint8_t> blocks;  // untiled, guest byte order
    std::vector<uint64_t> tile_hashes;
    std::unordered_map<uint64_t, uint32_t> unique_tiles;  // hash of a tile no other tile has -> tile
  };
  std::unordered_map<uint64_t, FontSource> font_sources;
  std::deque<std::pair<std::filesystem::path, std::shared_ptr<const texture_replacement::Image>>>
      images;

  std::shared_ptr<const texture_replacement::Image> CachedImage(const std::filesystem::path& path,
                                                                 std::string& error) {
    for (auto& entry : images) {
      if (entry.first == path) return entry.second;
    }
    auto image = std::make_shared<texture_replacement::Image>();
    if (!texture_replacement::LoadDds(path, *image, error)) return nullptr;
    if (images.size() >= 8) images.pop_front();
    images.emplace_back(path, image);
    return image;
  }

  const FontSource* GetFontSource(uint64_t hash) {
    auto it = font_sources.find(hash);
    if (it != font_sources.end()) return it->second.blocks.empty() ? nullptr : &it->second;
    FontSource source;
    const texture_replacement::Entry* entry = texture_replacement::Registry::Get().Find(hash);
    // The size is in the file name: <hash>_<w>x<h>_...
    uint32_t w = 0, h = 0;
    if (entry) {
      std::string stem = entry->path.stem().string();
      if (stem.size() > 17) std::sscanf(stem.c_str() + 17, "%ux%u", &w, &h);
    }
    if (entry && w && h && w % (texture_replacement::kFontTileBlocks * 4) == 0 &&
        h % (texture_replacement::kFontTileBlocks * 4) == 0 &&
        texture_replacement::LoadFontSource(entry->font_source, w, h, source.blocks)) {
      source.blocks_w = w / 4;
      source.blocks_h = h / 4;
      source.tiles_w = source.blocks_w / texture_replacement::kFontTileBlocks;
      source.tiles_h = source.blocks_h / texture_replacement::kFontTileBlocks;
      source.tile_hashes = texture_replacement::TileHashes(source.blocks, source.blocks_w, source.blocks_h);
      std::unordered_map<uint64_t, uint32_t> count;
      for (uint64_t t : source.tile_hashes) ++count[t];
      for (uint32_t i = 0; i < source.tile_hashes.size(); ++i) {
        if (count[source.tile_hashes[i]] == 1) source.unique_tiles[source.tile_hashes[i]] = i;
      }
    } else {
      if (entry && w && h && w % 128 == 0 && h % 128 == 0) {
        REXLOG_WARN("LR textures: {} - not a font atlas original", entry->font_source.string());
      }
      source.blocks.clear();
    }
    it = font_sources.emplace(hash, std::move(source)).first;
    return it->second.blocks.empty() ? nullptr : &it->second;
  }

  // Composites a font replacement into `job` if the guest texture shows font atlas tiles (see
  // "Font windows" in replacement.h): the texture's tiles that are an atlas's (a few unique tiles
  // anchor where each atlas is relative to the texture, then every tile is mapped by address)
  // come from the atlas's rebuilt replacement, with the blocks the game changed rebuilt from
  // guest memory; other tiles are the guest data enlarged.
  void TryFontAtlas(Job& job) {
    texture_replacement::Registry& registry = texture_replacement::Registry::Get();
    constexpr uint32_t kTile = texture_replacement::kFontTileBlocks;
    if (!registry.FontAtlases(job.width, job.height) || job.format != xenos::TextureFormat::k_DXT1 ||
        job.blocks_w % kTile || job.blocks_h % kTile) {
      return;
    }
    std::vector<uint8_t> guest;
    if (!texture_replacement::UntileBaseLevel(job.data, job.size, job.blocks_w, job.blocks_h, 8,
                                              job.pitch_blocks, job.tiled, guest)) {
      return;
    }
    if (const char* dump = std::getenv("LR_FONT_DUMP")) {
      char name[64];
      std::snprintf(name, sizeof(name), "/%08X_%016llX.bin", uint32_t(job.key.base_page) << 12,
                    (unsigned long long)job.raw_hash);
      if (FILE* f = std::fopen((std::string(dump) + name).c_str(), "wb")) {
        std::fwrite(guest.data(), 1, guest.size(), f);
        std::fclose(f);
      }
    }
    const uint32_t tiles_w = job.blocks_w / kTile, tiles_h = job.blocks_h / kTile;
    std::vector<uint64_t> tiles = texture_replacement::TileHashes(guest, job.blocks_w, job.blocks_h);
    static const uint8_t kFill[8] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    for (size_t i = 0; i + 8 <= guest.size(); i += 8) job.font_unfilled += !std::memcmp(&guest[i], kFill, 8);
    // Anchor each atlas: tile k of the texture is tile k - delta of the atlas (tiles are in memory
    // order in both, 8 KB each).
    struct Anchor {
      uint64_t hash;
      const FontSource* source;
      int64_t delta;
      uint32_t votes;
    };
    std::vector<Anchor> anchors;
    uint32_t matched_unique = 0;
    for (uint64_t hash : registry.AllFontAtlases()) {
      const FontSource* source = GetFontSource(hash);
      if (!source) continue;
      std::unordered_map<int64_t, uint32_t> votes;
      uint32_t hits = 0;
      for (uint32_t k = 0; k < tiles.size(); ++k) {
        auto u = source->unique_tiles.find(tiles[k]);
        if (u == source->unique_tiles.end()) continue;
        ++hits;
        // Tile indices in memory order: row-major with the texture's own tiles per row.
        int64_t atlas_mem = int64_t(u->second / source->tiles_w) * source->tiles_w + u->second % source->tiles_w;
        ++votes[int64_t(k) - atlas_mem];
      }
      auto best = std::max_element(votes.begin(), votes.end(),
                                   [](const auto& a, const auto& b) { return a.second < b.second; });
      // Only an atlas at the texture's own address (delta 0): a shifted match is not trusted -
      // with stale guest memory (GPU copies not read back yet) it maps glyphs to the wrong
      // places and the text becomes unreadable. A texture that holds the whole atlas matches
      // by hash anyway once guest memory is current.
      if (best != votes.end() && best->first == 0 && best->second >= 2 && best->second * 2 >= hits) {
        anchors.push_back({hash, source, best->first, best->second});
        matched_unique += best->second;
      }
    }
    job.font_equal = float(matched_unique) / float(tiles.size());
    if (anchors.empty()) return;
    // The rebuilt atlases (all at the same scale).
    std::vector<std::shared_ptr<const texture_replacement::Image>> images_of(anchors.size());
    uint32_t scale = 0;
    for (size_t a = 0; a < anchors.size(); ++a) {
      const texture_replacement::Entry* entry = registry.Find(anchors[a].hash);
      images_of[a] = entry ? CachedImage(entry->path, job.error) : nullptr;
      const texture_replacement::Image* image = images_of[a].get();
      uint32_t s = image && image->dxgi_format == texture_replacement::kDxgiBC1Unorm
                       ? image->width / (anchors[a].source->blocks_w * 4)
                       : 0;
      if (!s || (scale && s != scale)) return;
      scale = s;
    }
    static const bool test_scale1 = std::getenv("LR_TEST_FONT_SCALE1") != nullptr;
    if (test_scale1) scale = 1;
    uint32_t levels = 1;
    while ((1u << (levels - 1)) < scale) ++levels;  // down to the guest's own size
    if (const char* v = std::getenv("LR_TEST_FONT_LEVELS")) levels = std::max(1, std::atoi(v));
    texture_replacement::Image image =
        texture_replacement::MakeBc1Image(job.width * scale, job.height * scale, levels);
    std::vector<uint8_t> source_view = guest;  // what each block "should" be; differences are rebuilt
    std::string desc;
    std::vector<uint32_t> mapped(anchors.size(), 0);
    for (uint32_t ty = 0; ty < tiles_h; ++ty) {
      for (uint32_t tx = 0; tx < tiles_w; ++tx) {
        int64_t k = int64_t(ty) * tiles_w + tx;
        size_t a = 0;
        int64_t j = -1;
        for (; a < anchors.size(); ++a) {
          j = k - anchors[a].delta;
          if (j >= 0 && j < int64_t(anchors[a].source->tile_hashes.size())) break;
        }
        static const bool test_nearest = std::getenv("LR_TEST_FONT_NEAREST") != nullptr;
        if (a == anchors.size() || test_nearest) {
          texture_replacement::UpscaleGuestTile(guest, job.blocks_w, tx, ty, image, scale);
          continue;
        }
        const FontSource& source = *anchors[a].source;
        uint32_t sx = uint32_t(j) % source.tiles_w, sy = uint32_t(j) / source.tiles_w;
        texture_replacement::CopyImageTile(*images_of[a], sx, sy, image, tx, ty, kTile * 4 * scale);
        for (uint32_t y = 0; y < kTile; ++y) {
          std::memcpy(&source_view[(size_t(ty * kTile + y) * job.blocks_w + tx * kTile) * 8],
                      &source.blocks[(size_t(sy * kTile + y) * source.blocks_w + sx * kTile) * 8], kTile * 8);
        }
        ++mapped[a];
      }
    }
    for (size_t a = 0; a < anchors.size(); ++a) {
      const texture_replacement::Entry* entry = registry.Find(anchors[a].hash);
      std::string name = entry ? entry->path.filename().string() : std::string("?");
      size_t cut = name.rfind('_');
      desc += fmt::format("{}{} tiles of {} (at {:+#x})", desc.empty() ? "" : ", ", mapped[a],
                          cut != std::string::npos ? name.substr(cut + 1) : name, anchors[a].delta * 0x2000);
    }
    job.font_desc = std::move(desc);
    job.font_full = true;
    job.entry = registry.Find(anchors[0].hash);
    job.entry_hash = anchors[0].hash;
    job.composite = true;
    auto out = std::make_shared<texture_replacement::Image>(std::move(image));
    job.font_dirty = texture_replacement::FontComposite(*out, guest, source_view, job.width, job.height);
    if (const char* dump = std::getenv("LR_FONT_DUMP")) {
      char name[64];
      std::snprintf(name, sizeof(name), "/%08X_composite.dds", uint32_t(job.key.base_page) << 12);
      texture_replacement::SaveDds(std::string(dump) + name, out->width, out->height, out->dxgi_format,
                                   out->data.data(), out->levels[0].row_pitch * size_t(out->levels[0].rows));
    }
    job.image = std::move(out);
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Job> jobs, done;
  bool quit = false;
  // Jobs queued or in flight, by raw hash and texture (GPU thread only).
  std::unordered_set<uint64_t> pending;
  std::thread thread;

  LrReplaceQueue() : thread([this] { Run(); }) {}
  ~LrReplaceQueue() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      quit = true;
    }
    cv.notify_all();
    thread.join();
  }

  static uint64_t PendingKey(const TextureKey& key, uint64_t raw_hash) {
    return raw_hash ^ (uint64_t(TextureKey::Hasher()(key)) * UINT64_C(0x9E3779B97F4A7C15));
  }

  void Push(Job&& job) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      jobs.push_back(std::move(job));
    }
    cv.notify_one();
  }

  void Run() {
    texture_replacement::Registry& registry = texture_replacement::Registry::Get();
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return quit || !jobs.empty(); });
        if (quit) return;
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      if (!job.known) {
        job.result.hash = texture_replacement::HashBaseLevel(
            job.data, job.size, job.blocks_w, job.blocks_h, job.bytes_per_block, job.pitch_blocks,
            job.tiled);
        job.result.matched = 0;
        if (job.result.hash && !registry.Find(job.result.hash)) {
          // The other tiling, in case the game's copy is laid out unlike the fetch constant.
          uint64_t other = texture_replacement::HashBaseLevel(
              job.data, job.size, job.blocks_w, job.blocks_h, job.bytes_per_block,
              job.pitch_blocks, !job.tiled);
          if (other && registry.Find(other)) {
            job.result.matched = other;
          }
        }
        if (job.result.hash && !job.result.matched && !registry.Find(job.result.hash)) {
          TryFontAtlas(job);
        }
        if (job.result.hash && !job.composite && !job.result.matched &&
            !registry.Find(job.result.hash) && registry.HasSignatures(job.width, job.height)) {
          // No replacement under the game data's hash: try the content signatures of the
          // replacements of this size (see replacement.h).
          job.sig_tried = true;
          uint8_t sig[texture_replacement::kSigBytes];
          float best = 1e9f;
          bool block_compressed = FormatInfo::Get(job.format)->block_width != 1;
          // The fetch constant's tiling first, then the other one in case the game's copy
          // is laid out unlike the texture that samples it.
          for (int other_tiling = 0; other_tiling < 2; ++other_tiling) {
            bool tiled = other_tiling ? !job.tiled : job.tiled;
            for (int swap16 = 1; swap16 >= 0; --swap16) {
              if (!texture_replacement::GuestSignature(job.data, job.size, job.width, job.height,
                                                       job.format, job.pitch_blocks, tiled,
                                                       swap16 != 0, sig)) {
                break;
              }
              // A flat texture (cleared memory, a solid fill) says nothing about which texture
              // it is.
              int spread = 0;
              for (int c = 0; c < 4; ++c) {
                int lo = 255, hi = 0;
                for (size_t k = c; k < texture_replacement::kSigBytes; k += 4) {
                  lo = std::min(lo, int(sig[k]));
                  hi = std::max(hi, int(sig[k]));
                }
                spread = std::max(spread, hi - lo);
              }
              if (spread < 16) continue;
              float diff;
              uint64_t m = registry.MatchSignature(job.width, job.height, sig, &diff);
              if (diff < best) {
                best = diff;
                job.result.matched = m;
              }
              if (!block_compressed) break;  // byte order only matters for block-compressed data
            }
          }
          job.diff = best;
        }
        for (uint32_t k = 0; k + 8 <= job.size; k += 8) {
          uint64_t v;
          std::memcpy(&v, job.data + k, 8);
          job.fill_groups += v == UINT64_C(0xCCCCCCCCCCCCCCCC);
        }
        job.stale = XXH3_64bits(job.data, job.size) != job.raw_hash;
      }
      if (!job.stale && !job.composite) {
        job.entry_hash = registry.Find(job.result.hash) ? job.result.hash : job.result.matched;
        job.entry = job.entry_hash ? registry.Find(job.entry_hash) : nullptr;
        if (job.entry) {
          job.image = std::make_shared<texture_replacement::Image>();
          if (!texture_replacement::LoadDds(job.entry->path, *job.image, job.error)) {
            job.image.reset();
          }
        }
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        done.push_back(std::move(job));
      }
    }
  }
};

// Shader capture: the vertex + pixel shader pairs seen drawing a listed texture, with the number
// of such draws (capture/counts.txt).
struct TextureCache::LrCaptureState {
  std::mutex mutex;
  std::filesystem::path dir;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> draws;
  uint64_t total = 0;
  uint32_t logged = 0;
};

TextureCache::TextureCache(const RegisterFile& register_file, SharedMemory& shared_memory,
                           uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y)
    : register_file_(register_file),
      shared_memory_(shared_memory),
      draw_resolution_scale_x_(draw_resolution_scale_x),
      draw_resolution_scale_y_(draw_resolution_scale_y) {
  assert_true(draw_resolution_scale_x >= 1);
  assert_true(draw_resolution_scale_x <= kMaxDrawResolutionScaleAlongAxis);
  assert_true(draw_resolution_scale_y >= 1);
  assert_true(draw_resolution_scale_y <= kMaxDrawResolutionScaleAlongAxis);

  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    constexpr uint32_t kScaledResolvePageDwordCount = SharedMemory::kBufferSize / 4096 / 32;
    scaled_resolve_pages_ = std::unique_ptr<uint32_t[]>(new uint32_t[kScaledResolvePageDwordCount]);
    std::memset(scaled_resolve_pages_.get(), 0, kScaledResolvePageDwordCount * sizeof(uint32_t));
    std::memset(scaled_resolve_pages_l2_, 0, sizeof(scaled_resolve_pages_l2_));
    scaled_resolve_global_watch_handle_ =
        shared_memory.RegisterGlobalWatch(ScaledResolveGlobalWatchCallbackThunk, this);
  }

  if (texture_replacement::Registry::Get().capture_enabled()) {
    lr_capture_state_ = std::make_unique<LrCaptureState>();
    std::error_code ec;
    lr_capture_state_->dir = texture_replacement::Registry::Get().root() / "capture";
    std::filesystem::create_directories(lr_capture_state_->dir, ec);
    lr_capture_ = std::filesystem::is_directory(lr_capture_state_->dir, ec);
    if (!lr_capture_) {
      REXLOG_WARN("LR capture: cannot create {}", lr_capture_state_->dir.string());
    }
  }
}

TextureCache::~TextureCache() {
  lr_replace_queue_.reset();
  DestroyAllTextures(true);

  if (scaled_resolve_global_watch_handle_) {
    shared_memory().UnregisterGlobalWatch(scaled_resolve_global_watch_handle_);
  }
}

bool TextureCache::GetConfigDrawResolutionScale(uint32_t& x_out, uint32_t& y_out) {
  if (int32_t live = lr::resolution_scale_override().load(std::memory_order_relaxed); live > 0) {
    x_out = y_out = std::min(kMaxDrawResolutionScaleAlongAxis, uint32_t(live));
    return x_out == uint32_t(live);
  }
  uint32_t shared_scale = uint32_t(std::max(INT32_C(1), REXCVAR_GET(resolution_scale)));
  bool use_shared_scale = rex::cvar::HasNonDefaultValue("resolution_scale");
  uint32_t config_x = use_shared_scale && !rex::cvar::HasNonDefaultValue("draw_resolution_scale_x")
                          ? shared_scale
                          : uint32_t(std::max(INT32_C(1), REXCVAR_GET(draw_resolution_scale_x)));
  uint32_t config_y = use_shared_scale && !rex::cvar::HasNonDefaultValue("draw_resolution_scale_y")
                          ? shared_scale
                          : uint32_t(std::max(INT32_C(1), REXCVAR_GET(draw_resolution_scale_y)));
  uint32_t clamped_x = std::min(kMaxDrawResolutionScaleAlongAxis, config_x);
  uint32_t clamped_y = std::min(kMaxDrawResolutionScaleAlongAxis, config_y);
  x_out = clamped_x;
  y_out = clamped_y;
  return clamped_x == config_x && clamped_y == config_y;
}

void TextureCache::ClearCache() {
  DestroyAllTextures();
}

void TextureCache::CompletedSubmissionUpdated(uint64_t completed_submission_index) {
  while (!replacements_retired_.empty() &&
         replacements_retired_.front().first <= completed_submission_index) {
    replacements_retired_.pop_front();
  }
  // If memory usage is too high, destroy unused textures.
  uint64_t current_time = rex::chrono::Clock::QueryHostUptimeMillis();
  // texture_cache_memory_limit_render_to_texture is assumed to be included in
  // texture_cache_memory_limit_soft and texture_cache_memory_limit_hard, at 1x,
  // so subtracting 1 from the scale.
  uint32_t limit_scaled_resolve_add_mb =
      REXCVAR_GET(texture_cache_memory_limit_render_to_texture) *
      (draw_resolution_scale_x() * draw_resolution_scale_y() - 1);
  uint32_t limit_soft_mb =
      REXCVAR_GET(texture_cache_memory_limit_soft) + limit_scaled_resolve_add_mb;
  uint32_t limit_hard_mb =
      REXCVAR_GET(texture_cache_memory_limit_hard) + limit_scaled_resolve_add_mb;
  uint32_t limit_soft_lifetime = REXCVAR_GET(texture_cache_memory_limit_soft_lifetime) * 1000;
  bool destroyed_any = false;
  while (texture_used_first_ != nullptr) {
    uint64_t total_host_memory_usage_mb =
        (textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20;
    bool limit_hard_exceeded = total_host_memory_usage_mb > limit_hard_mb;
    if (total_host_memory_usage_mb <= limit_soft_mb && !limit_hard_exceeded) {
      break;
    }
    Texture* texture = texture_used_first_;
    if (texture->last_usage_submission_index() > completed_submission_index) {
      break;
    }
    if (!limit_hard_exceeded && (texture->last_usage_time() + limit_soft_lifetime) > current_time) {
      break;
    }
    if (!destroyed_any) {
      destroyed_any = true;
      // The texture being destroyed might have been bound in the previous
      // submissions, and nothing has overwritten the binding yet, so completion
      // of the submission where the texture was last actually used on the GPU
      // doesn't imply that it's not bound currently. Reset bindings if
      // any texture has been destroyed.
      ResetTextureBindings();
    }
    lr::frame_stats().evicted.fetch_add(1, std::memory_order_relaxed);
    // Remove the texture from the map and destroy it via its unique_ptr.
    auto found_texture_it = textures_.find(texture->key());
    assert_true(found_texture_it != textures_.end());
    if (found_texture_it != textures_.end()) {
      assert_true(found_texture_it->second.get() == texture);
      textures_.erase(found_texture_it);
      // `texture` is invalid now.
    }
  }
  if (destroyed_any) {
    COUNT_profile_set("gpu/texture_cache/textures", textures_.size());
  }
}

void TextureCache::BeginSubmission(uint64_t new_submission_index) {
  assert_true(new_submission_index > current_submission_index_);
  current_submission_index_ = new_submission_index;
  current_submission_time_ = rex::chrono::Clock::QueryHostUptimeMillis();
}

void TextureCache::BeginFrame() {
  // In case there was a failure to create something in the previous frame, make
  // sure bindings are reset so a new attempt will surely be made if the texture
  // is requested again.
  ResetTextureBindings();
  ++lr_frame_;
  LrApplyReplacementResults();
  LrRecheckReplacements();
  LrKeyboardRefresh();
  if (lr_bindings_changed_) {
    UpdateTextureBindingsImpl(lr_bindings_changed_);
    lr_bindings_changed_ = 0;
  }
}

void TextureCache::LrKeyboardRefresh() {
  texture_replacement::Registry& registry = texture_replacement::Registry::Get();
  uint32_t generation = registry.keyboard_generation();
  if (generation == lr_keyboard_generation_) return;
  lr_keyboard_generation_ = generation;
  uint32_t refreshed = 0;
  for (auto& [key, texture] : textures_) {
    if (!texture || texture->is_replacement() || !registry.KeyboardSize(key.GetWidth(), key.GetHeight())) {
      continue;
    }
    LrRetireReplacement(*texture);
    texture->replacement_hash_ = 0;
    texture->lr_raw_hash_ = 0;
    UpdateReplacement(*texture);
    ++refreshed;
  }
  texture_bindings_in_sync_ = 0;
  REXLOG_INFO("LR textures: keyboard icons {} - {} texture(s) re-resolved", registry.keyboard_on() ? "on" : "off",
              refreshed);
}

void TextureCache::LrRecheckReplacements() {
  lr::ScopedTime lr_time(lr::frame_stats().replace_us);
  if (lr_recheck_.empty()) return;
  auto start = std::chrono::steady_clock::now();
  size_t visited = 0;
  while (visited < lr_recheck_.size() &&
         std::chrono::steady_clock::now() - start < std::chrono::microseconds(2000)) {
    ++visited;
    if (lr_recheck_next_ >= lr_recheck_.size()) lr_recheck_next_ = 0;
    LrRecheck& entry = lr_recheck_[lr_recheck_next_];
    auto it = textures_.find(entry.key);
    bool drop = it == textures_.end() || it->second->replacement_ != nullptr;
    if (!drop && lr_frame_ - entry.last_frame >= 60) {
      entry.last_frame = lr_frame_;
      lr_rechecking_ = true;
      UpdateReplacement(*it->second);
      lr_rechecking_ = false;
      drop = it->second->replacement_ != nullptr;
    }
    if (drop) {
      lr_recheck_.erase(lr_recheck_.begin() + lr_recheck_next_);
    } else {
      ++lr_recheck_next_;
    }
  }
}

void TextureCache::MarkRangeAsResolved(uint32_t start_unscaled, uint32_t length_unscaled) {
  if (length_unscaled == 0) {
    return;
  }
  start_unscaled &= 0x1FFFFFFF;
  length_unscaled = std::min(length_unscaled, 0x20000000 - start_unscaled);

  if (IsDrawResolutionScaled()) {
    uint32_t page_first = start_unscaled >> 12;
    uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
    uint32_t block_first = page_first >> 5;
    uint32_t block_last = page_last >> 5;
    LR_GLOBAL_LOCK();
    for (uint32_t i = block_first; i <= block_last; ++i) {
      uint32_t add_bits = UINT32_MAX;
      if (i == block_first) {
        add_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (i == block_last && (page_last & 31) != 31) {
        add_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      scaled_resolve_pages_[i] |= add_bits;
      scaled_resolve_pages_l2_[i >> 6] |= UINT64_C(1) << (i & 63);
    }
  }

  // Invalidate textures. Toggling individual textures between scaled and
  // unscaled also relies on invalidation through shared memory.
  shared_memory().RangeWrittenByGpu(start_unscaled, length_unscaled);
  if (lr::fx_trace().on) lr::fx_event(fmt::format("resolve {:08X}+{:X}", start_unscaled, length_unscaled));
}

uint32_t TextureCache::GuestToHostSwizzle(uint32_t guest_swizzle, uint32_t host_format_swizzle) {
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t guest_swizzle_component = (guest_swizzle >> (3 * i)) & 0b111;
    uint32_t host_swizzle_component;
    if (guest_swizzle_component >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) {
      // Get rid of 6 and 7 values (to prevent host GPU errors if the game has
      // something broken) the simple way - by changing them to 4 (0) and 5 (1).
      host_swizzle_component = guest_swizzle_component & 0b101;
    } else {
      host_swizzle_component = (host_format_swizzle >> (3 * guest_swizzle_component)) & 0b111;
    }
    host_swizzle |= host_swizzle_component << (3 * i);
  }
  return host_swizzle;
}

bool TextureCache::PrepareTextureLoad(Texture& texture, PendingTextureLoad& pending_load_out,
                                      PendingSharedMemoryRange* pending_ranges_out,
                                      size_t& pending_range_count_out) {
  uint32_t outdated_mask = texture.outdated_mask();
  if (!outdated_mask) {
    return false;
  }

  bool base_outdated = false;
  bool mips_outdated = false;
  {
    LR_GLOBAL_LOCK();
    if (outdated_mask & Texture::kOutdatedBitBase) {
      base_outdated = texture.base_outdated(global_lock);
    }
    if (outdated_mask & Texture::kOutdatedBitMips) {
      mips_outdated = texture.mips_outdated(global_lock);
    }
  }
  if (!base_outdated && !mips_outdated) {
    return false;
  }

  pending_load_out.texture = &texture;
  pending_load_out.load_base = base_outdated;
  pending_load_out.load_mips = mips_outdated;
  pending_range_count_out = 0;

  TextureKey texture_key = texture.key();
  if (base_outdated) {
    PendingSharedMemoryRange pending_range;
    pending_range.start = texture_key.base_page << 12;
    pending_range.length =
        static_cast<uint32_t>(rex::align(texture.GetGuestBaseSize(), UINT32_C(16)));
    pending_ranges_out[pending_range_count_out++] = pending_range;
  }
  if (mips_outdated) {
    PendingSharedMemoryRange pending_range;
    pending_range.start = texture_key.mip_page << 12;
    pending_range.length =
        static_cast<uint32_t>(rex::align(texture.GetGuestMipsSize(), UINT32_C(16)));
    pending_ranges_out[pending_range_count_out++] = pending_range;
  }

  return true;
}

bool TextureCache::CommitPreparedTextureLoad(const PendingTextureLoad& pending_load) {
  if (!pending_load.texture || (!pending_load.load_base && !pending_load.load_mips)) {
    return true;
  }

  Texture& texture = *pending_load.texture;
  TextureKey texture_key = texture.key();
  if (texture_key.scaled_resolve) {
    // Make sure all the scaled resolve memory is resident and accessible from
    // the shader, including any possible padding that hasn't yet been touched
    // by an actual resolve, but is still included in the texture size, so the
    // GPU won't be trying to access unmapped memory.
    if (pending_load.load_base && !EnsureScaledResolveMemoryCommitted(
                                      texture_key.base_page << 12, texture.GetGuestBaseSize(), 4)) {
      return false;
    }
    if (pending_load.load_mips && !EnsureScaledResolveMemoryCommitted(
                                      texture_key.mip_page << 12, texture.GetGuestMipsSize(), 4)) {
      return false;
    }
  }

  if (!LoadTextureDataFromResidentMemoryImpl(texture, pending_load.load_base,
                                             pending_load.load_mips)) {
    return false;
  }

  // Mark the ranges as uploaded and watch them. This is needed for scaled
  // resolves as well to detect when the CPU wants to reuse the memory for a
  // regular texture or a vertex buffer, and thus the scaled resolve version is
  // not up to date anymore.
  texture.MakeUpToDateAndWatch(global_critical_region_.Acquire());
  texture.LogAction("Loaded");
  if (lr::fx_trace().on) {
    lr::fx_event(fmt::format("load {:08X} {}x{}", texture_key.base_page << 12, texture_key.GetWidth(),
                             texture_key.GetHeight()));
  }
  lr::frame_stats().textures.fetch_add(1, std::memory_order_relaxed);
  lr::frame_stats().texture_bytes.fetch_add(
      (pending_load.load_base ? texture.GetGuestBaseSize() : 0) +
          (pending_load.load_mips ? texture.GetGuestMipsSize() : 0),
      std::memory_order_relaxed);

  if (pending_load.load_base) {
    UpdateReplacement(texture);
  }

  return true;
}

void TextureCache::RequestTextures(uint32_t used_texture_mask) {
  lr::ScopedTime lr_time(lr::frame_stats().texture_us);
  const auto& regs = register_file();

  // UI style switched (PS3 / 360 UI textures): resolve every binding again so replacements
  // are bound or unbound.
  uint32_t style_generation = texture_replacement::Registry::Get().style_generation();
  if (style_generation != replacement_style_generation_) {
    replacement_style_generation_ = style_generation;
    texture_bindings_in_sync_ = 0;
    // Textures loaded while the 360 style was on were never copied back or hashed for the PS3
    // UI set (UpdateReplacement skips them then), and a UI sheet the game builds once is not
    // loaded again: look at every texture again now.
    if (texture_replacement::Registry::Get().ps3_ui()) {
      for (auto& entry : textures_) {
        Texture& texture = *entry.second;
        if (!texture.replacement_ && !texture.is_replacement()) {
          UpdateReplacement(texture);
        }
      }
    }
  }

  if (texture_became_outdated_.exchange(false, std::memory_order_acquire)) {
    // A texture has become outdated - make sure whether textures are outdated
    // is rechecked in this draw and in subsequent ones to reload the new data
    // if needed.
    ResetTextureBindings();
  }

  // Update the texture keys and the textures.
  uint32_t bindings_changed = 0;
  std::vector<PendingTextureLoad> pending_texture_loads;
  std::vector<PendingSharedMemoryRange> pending_shared_memory_ranges;
  auto queue_pending_texture_load = [&](Texture* texture) {
    if (texture == nullptr) {
      return;
    }
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      if (pending_load.texture == texture) {
        return;
      }
    }
    PendingTextureLoad pending_load;
    PendingSharedMemoryRange pending_ranges[2];
    size_t pending_range_count = 0;
    if (!PrepareTextureLoad(*texture, pending_load, pending_ranges, pending_range_count)) {
      return;
    }
    pending_texture_loads.push_back(pending_load);
    for (size_t i = 0; i < pending_range_count; ++i) {
      pending_shared_memory_ranges.push_back(pending_ranges[i]);
    }
  };
  uint32_t textures_remaining = used_texture_mask & ~texture_bindings_in_sync_;
  const uint32_t textures_updated = textures_remaining;
  uint32_t index = 0;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    uint32_t index_bit = UINT32_C(1) << index;
    textures_remaining &= ~index_bit;
    TextureBinding& binding = texture_bindings_[index];
    xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(index);
    TextureKey old_key = binding.key;
    uint8_t old_swizzled_signs = binding.swizzled_signs;
    BindingInfoFromFetchConstant(fetch, binding.key, &binding.swizzled_signs);
    texture_bindings_in_sync_ |= index_bit;
    {
      // LR_TEXTURE_LOG=fetch logs each distinct raw fetch constant once.
      static const bool lr_log_fetch = [] {
        const char* v = std::getenv("LR_TEXTURE_LOG");
        return v && std::strcmp(v, "fetch") == 0;
      }();
      if (lr_log_fetch) {
        static std::unordered_set<uint64_t> seen;
        const uint32_t* d = reinterpret_cast<const uint32_t*>(&fetch);
        uint64_t id = XXH3_64bits(d, sizeof(fetch));
        if (seen.insert(id).second) {
          REXLOG_INFO("LR fetch {}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} -> base {:08X} {}x{}", index, d[0],
                      d[1], d[2], d[3], d[4], d[5], uint32_t(binding.key.base_page) << 12, binding.key.GetWidth(),
                      binding.key.GetHeight());
        }
      }
    }
    if (!binding.key.is_valid) {
      if (old_key.is_valid) {
        bindings_changed |= index_bit;
      }
      binding.Reset();
      continue;
    }
    uint32_t old_host_swizzle = binding.host_swizzle;
    binding.host_swizzle = GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(binding.key));

    // Check if need to load the unsigned and the signed versions of the texture
    // (if the format is emulated with different host bit representations for
    // signed and unsigned - otherwise only the unsigned one is loaded).
    bool key_changed = binding.key != old_key;
    bool any_sign_was_not_signed = texture_util::IsAnySignNotSigned(old_swizzled_signs);
    bool any_sign_was_signed = texture_util::IsAnySignSigned(old_swizzled_signs);
    bool any_sign_is_not_signed = texture_util::IsAnySignNotSigned(binding.swizzled_signs);
    bool any_sign_is_signed = texture_util::IsAnySignSigned(binding.swizzled_signs);
    if (key_changed || binding.host_swizzle != old_host_swizzle ||
        any_sign_is_not_signed != any_sign_was_not_signed ||
        any_sign_is_signed != any_sign_was_signed) {
      bindings_changed |= index_bit;
    }
    bool load_unsigned_data = false, load_signed_data = false;
    if (IsSignedVersionSeparateForFormat(binding.key)) {
      // Can reuse previously loaded unsigned/signed versions if the key is the
      // same and the texture was previously bound as unsigned/signed
      // respectively (checking the previous values of signedness rather than
      // binding.texture != nullptr and binding.texture_signed != nullptr also
      // prevents repeated attempts to load the texture if it has failed to
      // load).
      if (any_sign_is_not_signed) {
        if (key_changed || !any_sign_was_not_signed) {
          binding.texture = FindOrCreateTexture(binding.key);
          load_unsigned_data = true;
        }
      } else {
        binding.texture = nullptr;
      }
      if (any_sign_is_signed) {
        if (key_changed || !any_sign_was_signed) {
          TextureKey signed_key = binding.key;
          signed_key.signed_separate = 1;
          binding.texture_signed = FindOrCreateTexture(signed_key);
          load_signed_data = true;
        }
      } else {
        binding.texture_signed = nullptr;
      }
    } else {
      // Same resource for both unsigned and signed, but descriptor formats may
      // be different.
      if (key_changed) {
        binding.texture = FindOrCreateTexture(binding.key);
        load_unsigned_data = true;
      }
      binding.texture_signed = nullptr;
    }
    if (load_unsigned_data) {
      queue_pending_texture_load(binding.texture);
    }
    if (load_signed_data) {
      queue_pending_texture_load(binding.texture_signed);
    }
  }

  COUNT_profile_set("gpu/texture_cache/request_textures_pending_load_count",
                    uint32_t(pending_texture_loads.size()));
  COUNT_profile_set("gpu/texture_cache/request_textures_pending_range_count",
                    uint32_t(pending_shared_memory_ranges.size()));

  bool batched_shared_memory_request_succeeded = true;
  std::vector<std::pair<uint32_t, uint32_t>> pending_shared_memory_range_pairs;
  if (!pending_shared_memory_ranges.empty()) {
    pending_shared_memory_range_pairs.reserve(pending_shared_memory_ranges.size());
    for (const PendingSharedMemoryRange& pending_range : pending_shared_memory_ranges) {
      pending_shared_memory_range_pairs.emplace_back(pending_range.start, pending_range.length);
    }
    batched_shared_memory_request_succeeded = shared_memory().RequestRanges(
        pending_shared_memory_range_pairs.data(), pending_shared_memory_range_pairs.size());
  }

  if (batched_shared_memory_request_succeeded) {
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      CommitPreparedTextureLoad(pending_load);
    }
  } else {
    for (const PendingTextureLoad& pending_load : pending_texture_loads) {
      if (pending_load.texture != nullptr) {
        LoadTextureData(*pending_load.texture);
      }
    }
  }
  // Bind replacements in place of the guest textures they stand in for.
  uint32_t replaced_remaining = textures_updated;
  while (rex::bit_scan_forward(replaced_remaining, &index)) {
    replaced_remaining &= ~(UINT32_C(1) << index);
    TextureBinding& binding = texture_bindings_[index];
    if (binding.texture && binding.texture->replacement() &&
        texture_replacement::Registry::Get().SetActive(binding.texture->replacement_set())) {
      binding.texture = binding.texture->replacement();
      bindings_changed |= UINT32_C(1) << index;
    }
  }
  bindings_changed |= lr_bindings_changed_;
  lr_bindings_changed_ = 0;
  if (bindings_changed) {
    UpdateTextureBindingsImpl(bindings_changed);
  }
}

void TextureCache::UpdateReplacement(Texture& texture) {
  lr::ScopedTime lr_time(lr::frame_stats().replace_us);
  texture_replacement::Registry& registry = texture_replacement::Registry::Get();
  if (texture.is_replacement() ||
      (!registry.replace_enabled() && !registry.dump_enabled() && !registry.capture_enabled())) {
    return;
  }
  TextureKey key = texture.key();
  uint32_t base_size = texture.GetGuestBaseSize();
  if (key.scaled_resolve || key.signed_separate ||
      key.dimension != xenos::DataDimension::k2DOrStacked || key.GetDepthOrArraySize() != 1 ||
      !base_size) {
    return;
  }
  // LR_TEXTURE_LOG=all logs every texture once (base, size, format), for finding what a screen
  // samples.
  static const bool lr_log_all = [] {
    const char* v = std::getenv("LR_TEXTURE_LOG");
    return v && std::strcmp(v, "all") == 0;
  }();
  if (lr_log_all) {
    static std::mutex seen_mutex;
    static std::unordered_set<uint64_t> seen;
    uint64_t id = (uint64_t(key.base_page) << 32) ^ (uint64_t(key.GetWidth()) << 16) ^ key.GetHeight() ^
                  (uint64_t(key.format) << 56);
    std::lock_guard<std::mutex> lock(seen_mutex);
    if (seen.insert(id).second) {
      REXLOG_INFO("LR texture: {:08X} {}x{} {} tiled {} endian {}", uint32_t(key.base_page) << 12,
                  key.GetWidth(), key.GetHeight(), FormatInfo::Get(key.format)->name, uint32_t(key.tiled),
                  uint32_t(key.endianness));
    }
  }
  // Hash only textures a replacement could match (the per-frame render-to-texture reloads
  // would otherwise be hashed every frame).
  if (!registry.WantsHash(key.GetWidth(), key.GetHeight())) {
    return;
  }
  const uint8_t* guest_data =
      shared_memory().guest_memory().TranslatePhysical<const uint8_t*>(key.base_page << 12);
  if (!guest_data) {
    return;
  }
  // Never read past the end of physical memory.
  base_size = std::min(base_size, uint32_t(0x20000000u - (uint32_t(key.base_page) << 12)));
  // The game wrote this texture (or part of it) with a shader memexport copy, which only
  // reached the GPU's copy of memory: have it copied back to guest memory first. When the
  // copy arrives (a frame or two later) the texture is hashed again (LrCopyBackDone); until
  // then whatever is bound stays.
  uint32_t lr_base = uint32_t(key.base_page) << 12;
  if (int copy_back = shared_memory().LrCopyBackState(lr_base, base_size)) {
    // Copied back (to be hashed) only for a replacement that can apply: a user replacement, or
    // the PS3 UI set while PS3 prompts are on (the game copies some UI sheets with memexport).
    // Otherwise the texture stays as the GPU has it: copying back render-to-texture pictures
    // for nothing (the battle-exit picture) raced the GPU's next write and made them flicker.
    if (!registry.has_user() && !registry.ps3_ui()) {
      return;
    }
    uint64_t range = (uint64_t(lr_base) << 32) | base_size;
    if (!registry.FontAtlases(key.GetWidth(), key.GetHeight()) && !lr_copy_back_matched_.count(range)) {
      uint8_t& tries = lr_copy_back_tries_[range];
      if (tries >= kLrCopyBackTries) {
        return;  // never matched: left as the GPU has it, no copy-back, no hash
      }
      if (copy_back == 2) {
        ++tries;
      }
    }
    if (copy_back == 2) {
      bool requested = false;
      for (const auto& request : lr_readback_requests_) {
        requested |= request.first == lr_base && request.second == base_size;
      }
      if (!requested) {
        lr_readback_requests_.emplace_back(lr_base, base_size);
      }
    }
    bool waiting = false;
    for (const LrCopyBackWait& wait : lr_copy_back_waiting_) {
      waiting |= wait.key == key;
    }
    if (!waiting) {
      if (lr_copy_back_waiting_.size() >= 1024) {
        lr_copy_back_waiting_.erase(lr_copy_back_waiting_.begin());
      }
      lr_copy_back_waiting_.push_back({key, lr_base, base_size});
    }
    return;
  }
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  uint32_t block_w = format_info->block_width, block_h = format_info->block_height;
  uint32_t bytes_per_block = format_info->bytes_per_block();
  uint32_t blocks_w = (key.GetWidth() + block_w - 1) / block_w;
  uint32_t blocks_h = (key.GetHeight() + block_h - 1) / block_h;
  uint32_t pitch_blocks = ((key.pitch << 5) + block_w - 1) / block_w;
  if (registry.dump_enabled()) {
    uint64_t hash = texture_replacement::HashBaseLevel(guest_data, base_size, blocks_w, blocks_h,
                                                       bytes_per_block, pitch_blocks, key.tiled != 0);
    if (hash && registry.ClaimDump(hash)) {
      DumpTextureBase(texture, hash, guest_data);
    }
    if (!registry.replace_enabled() && !registry.capture_enabled()) {
      return;
    }
  }
  uint64_t raw_hash = XXH3_64bits(guest_data, base_size);
  if (lr_rechecking_ && raw_hash == texture.lr_raw_hash_) {
    return;  // unchanged since it was last checked
  }
  texture.lr_raw_hash_ = raw_hash;
  if (texture.lr_composite_ && texture.replacement_) {
    // The game changed a font atlas: draw the original until the rebuilt replacement is ready
    // rather than glyphs that are no longer there.
    LrRetireReplacement(texture);
    texture.replacement_hash_ = 0;
  }
  texture.lr_composite_ = false;
  auto known = lr_raw_results_.find(raw_hash);
  if (known == lr_raw_results_.end()) {
    LrQueueReplacement(texture, raw_hash, nullptr);
    return;
  }
  const LrRawResult& result = known->second;
  if (!result.hash || result.hash == texture.replacement_hash_) {
    return;
  }
  if (texture.replacement_) {
    // The guest reused the memory for different data; the GPU may still be reading the
    // old replacement.
    LrRetireReplacement(texture);
  }
  texture.replacement_hash_ = result.hash;
  if (registry.Find(result.hash) || (result.matched && registry.Find(result.matched))) {
    LrQueueReplacement(texture, raw_hash, &result);
  }
}

void TextureCache::LrCaptureListedDraw(const Shader* vertex_shader, const Shader* pixel_shader) {
  if (!vertex_shader || !lr_capture_state_) {
    return;
  }
  const texture_replacement::Registry& registry = texture_replacement::Registry::Get();
  // The hash of a bound texture; a bound replacement stands for its guest texture. Hashes are
  // computed off the GPU thread, so a texture's first draws after it loads may not match yet.
  auto hash_of = [](const Texture* texture) -> uint64_t {
    if (!texture) {
      return 0;
    }
    if (texture->replacement_source_) {
      texture = texture->replacement_source_;
    }
    return texture->replacement_hash_;
  };
  auto listed = [&](const TextureBinding& binding) {
    return registry.CaptureHash(hash_of(binding.texture)) ||
           registry.CaptureHash(hash_of(binding.texture_signed));
  };
  bool any_listed = false;
  for (const Shader* shader : {vertex_shader, pixel_shader}) {
    if (!shader) {
      continue;
    }
    for (const Shader::TextureBinding& shader_binding : shader->texture_bindings()) {
      if (shader_binding.fetch_constant < texture_bindings_.size() &&
          listed(texture_bindings_[shader_binding.fetch_constant])) {
        any_listed = true;
      }
    }
  }
  if (!any_listed) {
    return;
  }

  LrCaptureState& state = *lr_capture_state_;
  std::lock_guard<std::mutex> lock(state.mutex);
  uint64_t vs_hash = vertex_shader->ucode_data_hash();
  uint64_t ps_hash = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  uint64_t& count = state.draws[{vs_hash, ps_hash}];
  bool first = count++ == 0;
  ++state.total;
  std::error_code ec;
  if (first || state.total % 4096 == 0) {
    std::ofstream counts(state.dir / "counts.txt", std::ios::trunc);
    counts << "# vertex shader, pixel shader, draws with a listed texture\n";
    for (const auto& [pair, draws] : state.draws) {
      counts << fmt::format("{:016X} {:016X} {}\n", pair.first, pair.second, draws);
    }
  }
  if (!first) {
    return;
  }

  const RegisterFile& regs = register_file();
  std::string summary = fmt::format("{:016X} {:016X}", vs_hash, ps_hash);
  std::string report = fmt::format(
      "LR shader capture\nvertex shader {:016X}\npixel shader {:016X}\n\n"
      "textures (stage fetch constant: replacement hash, * = listed; format, size, host swizzle; "
      "fetch constant dwords)\n",
      vs_hash, ps_hash);
  for (const Shader* shader : {vertex_shader, pixel_shader}) {
    if (!shader) {
      continue;
    }
    char stage = shader == vertex_shader ? 'V' : 'P';
    uint32_t done_mask = 0;
    for (const Shader::TextureBinding& shader_binding : shader->texture_bindings()) {
      uint32_t index = shader_binding.fetch_constant;
      if (index >= texture_bindings_.size() || (done_mask & (UINT32_C(1) << index))) {
        continue;
      }
      done_mask |= UINT32_C(1) << index;
      const TextureBinding& binding = texture_bindings_[index];
      xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(index);
      const uint32_t* d = reinterpret_cast<const uint32_t*>(&fetch);
      uint64_t hash = hash_of(binding.texture);
      bool is_listed = listed(binding);
      std::string texture =
          binding.key.is_valid
              ? fmt::format("{:016X}{} {} {}x{} tiled {} swizzle {:03X}", hash, is_listed ? "*" : "",
                            FormatInfo::Get(binding.key.format)->name, binding.key.GetWidth(),
                            binding.key.GetHeight(), uint32_t(binding.key.tiled), binding.host_swizzle)
              : std::string("none");
      report += fmt::format("{} {:2}: {}; {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}\n", stage, index, texture,
                            d[0], d[1], d[2], d[3], d[4], d[5]);
      summary += fmt::format(" {}{}={:016X}{}", stage, index, hash, is_listed ? "*" : "");
    }
  }
  // Constants as of this first draw (the shader's float constants, the bool constants).
  for (const Shader* shader : {vertex_shader, pixel_shader}) {
    if (!shader) {
      continue;
    }
    char stage = shader == vertex_shader ? 'V' : 'P';
    const Shader::ConstantRegisterMap& map = shader->constant_register_map();
    uint32_t base =
        shader == vertex_shader ? XE_GPU_REG_SHADER_CONSTANT_000_X : XE_GPU_REG_SHADER_CONSTANT_256_X;
    report += fmt::format("\n{} float constants\n", stage == 'V' ? "vertex" : "pixel");
    for (uint32_t c = 0; c < 256; ++c) {
      if (!(map.float_bitmap[c >> 6] & (uint64_t(1) << (c & 63)))) {
        continue;
      }
      const float* v = reinterpret_cast<const float*>(&regs[base + c * 4]);
      report += fmt::format("c{} {} {} {} {}\n", c, v[0], v[1], v[2], v[3]);
    }
  }
  report += "\nbool constants";
  for (uint32_t i = 0; i < 8; ++i) {
    report += fmt::format(" {:08X}", regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i]);
  }
  report += "\n";
  for (const Shader* shader : {vertex_shader, pixel_shader}) {
    if (shader) {
      report += fmt::format("\n----- {} shader {:016X}\n", shader == vertex_shader ? "vertex" : "pixel",
                            shader->ucode_data_hash());
      report += shader->ucode_disassembly();
    }
  }
  {
    std::ofstream file(state.dir / fmt::format("{:016X}_{:016X}.txt", vs_hash, ps_hash), std::ios::trunc);
    file << report;
  }
  {
    std::ofstream file(state.dir / "summary.txt", std::ios::app);
    file << summary << "\n";
  }
  if (state.logged < 50) {
    ++state.logged;
    REXLOG_INFO("LR capture: {}", summary);
  }
}

void TextureCache::LrCopyBackDone(uint32_t start, uint32_t length) {
  uint32_t end = start + length;
  std::vector<TextureKey> ready;
  for (size_t i = 0; i < lr_copy_back_waiting_.size();) {
    const LrCopyBackWait& wait = lr_copy_back_waiting_[i];
    // Any overlap: a texture that waited on someone else's copy (state 1) may span more than
    // that copy's range and would otherwise never be woken. UpdateReplacement waits again if
    // part of it is still stale or in flight.
    if (wait.start >= end || wait.start + wait.length <= start) {
      ++i;
      continue;
    }
    ready.push_back(wait.key);
    lr_copy_back_waiting_.erase(lr_copy_back_waiting_.begin() + i);
  }
  for (const TextureKey& key : ready) {
    auto it = textures_.find(key);
    if (it != textures_.end()) {
      // May wait again if memexport wrote there since.
      UpdateReplacement(*it->second);
    }
  }
}

void TextureCache::LrQueueReplacement(Texture& texture, uint64_t raw_hash,
                                      const LrRawResult* known) {
  if (!lr_replace_queue_) {
    lr_replace_queue_ = std::make_unique<LrReplaceQueue>();
  }
  TextureKey key = texture.key();
  uint64_t pending_key = LrReplaceQueue::PendingKey(key, raw_hash);
  if (!lr_replace_queue_->pending.insert(pending_key).second) {
    return;
  }
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  uint32_t block_w = format_info->block_width, block_h = format_info->block_height;
  LrReplaceQueue::Job job;
  job.key = key;
  job.raw_hash = raw_hash;
  job.data =
      shared_memory().guest_memory().TranslatePhysical<const uint8_t*>(key.base_page << 12);
  job.size = std::min(texture.GetGuestBaseSize(),
                      uint32_t(0x20000000u - (uint32_t(key.base_page) << 12)));
  job.width = key.GetWidth();
  job.height = key.GetHeight();
  job.blocks_w = (job.width + block_w - 1) / block_w;
  job.blocks_h = (job.height + block_h - 1) / block_h;
  job.bytes_per_block = format_info->bytes_per_block();
  job.pitch_blocks = ((key.pitch << 5) + block_w - 1) / block_w;
  job.format = key.format;
  job.tiled = key.tiled != 0;
  job.recheck = lr_rechecking_;
  if (known) {
    job.known = true;
    job.result = *known;
  }
  lr_replace_queue_->Push(std::move(job));
}

void TextureCache::LrApplyReplacementResults() {
  lr::ScopedTime lr_time(lr::frame_stats().replace_us);
  if (!lr_replace_queue_) {
    return;
  }
  std::deque<LrReplaceQueue::Job> done;
  {
    std::lock_guard<std::mutex> lock(lr_replace_queue_->mutex);
    done.swap(lr_replace_queue_->done);
  }
  static const bool lr_texlog = std::getenv("LR_TEXTURE_LOG") != nullptr;
  for (LrReplaceQueue::Job& job : done) {
    lr_replace_queue_->pending.erase(LrReplaceQueue::PendingKey(job.key, job.raw_hash));
    if (job.stale) {
      continue;
    }
    if (!job.known) {
      if (lr_raw_results_.size() >= 65536) {
        lr_raw_results_.clear();
      }
      // A font atlas is composited from guest memory each time, so its result is not reused.
      if (!job.composite) {
        lr_raw_results_[job.raw_hash] = job.result;
      }
      if (job.font_equal >= 0.0f && !job.composite) {
        static uint32_t font_logged = 0;
        if (font_logged < 40) {
          ++font_logged;
          if (!job.font_full) {
            REXLOG_INFO("LR textures: {:016X} {}x{} at {:08X}: not a font atlas ({:.0f}% of a sample of "
                        "the original glyph blocks; {} groups of unloaded memory)",
                        job.result.hash, job.width, job.height, uint32_t(job.key.base_page) << 12,
                        job.font_equal * 100.0f, job.fill_groups);
          } else {
            REXLOG_INFO("LR textures: {:016X} {}x{} at {:08X}: not a font atlas ({:.0f}% of the glyph "
                        "blocks of the closest original{}; {} blocks differ, {} of them empty memory)",
                        job.result.hash, job.width, job.height, uint32_t(job.key.base_page) << 12,
                        job.font_equal * 100.0f,
                        job.font_layout ? fmt::format(", layout variant {}", job.font_layout) : std::string(),
                        job.font_changed, job.font_unfilled);
          }
        }
      }
      if (lr_texlog) {
        REXLOG_INFO("LR texlog: {:016X} base {:08X} size {} {}x{} {} tiled {} pitch {} endian {}",
                    job.result.hash, uint32_t(job.key.base_page) << 12, job.size, job.width,
                    job.height, FormatInfo::Get(job.format)->name, uint32_t(job.tiled),
                    job.pitch_blocks, uint32_t(job.key.endianness));
      }
      if (job.sig_tried) {
        static uint32_t content_logged = 0, recheck_logged = 0;
        uint32_t& logged = job.recheck ? recheck_logged : content_logged;
        if (logged < (job.recheck ? 60u : 150u)) {
          ++logged;
          REXLOG_INFO("LR textures: {}{:016X} {}x{} {} at {:08X} (tiled {}, pitch {}): no "
                      "replacement by hash; by content {} (difference {:.1f})",
                      job.recheck ? "recheck " : "", job.result.hash, job.width, job.height,
                      FormatInfo::Get(job.format)->name, uint32_t(job.key.base_page) << 12,
                      uint32_t(job.tiled), job.pitch_blocks,
                      job.result.matched ? fmt::format("{:016X}", job.result.matched)
                      : job.diff > 1e8f  ? std::string("not compared, flat texture")
                                         : std::string("none"),
                      job.diff > 1e8f ? 0.0f : job.diff);
        }
      }
    }
    auto it = textures_.find(job.key);
    if (it == textures_.end()) {
      continue;
    }
    Texture& texture = *it->second;
    if (texture.lr_raw_hash_ != job.raw_hash || !job.result.hash) {
      continue;  // reloaded with other contents since
    }
    if (job.known) {
      if (texture.replacement_hash_ != job.result.hash || texture.replacement_) {
        continue;
      }
    } else {
      if (job.result.hash == texture.replacement_hash_) {
        continue;  // same contents as before
      }
      if (texture.replacement_) {
        LrRetireReplacement(texture);
      }
      texture.replacement_hash_ = job.result.hash;
    }
    if (!job.entry) {
      if (job.sig_tried || job.font_equal >= 0.0f) {
        // The game may fill the memory later without the renderer reloading the texture.
        bool listed = false;
        for (const LrRecheck& r : lr_recheck_) listed |= r.key == job.key;
        if (!listed) {
          // Kept when the list is full: textures that look not loaded yet (fill pattern) or
          // close to a font atlas - not every world texture of a replacement's size.
          bool font = job.fill_groups * 200u > job.size / 8u ||
                      (job.font_equal >= 0.0f && (job.font_unfilled > 0 || job.font_equal >= 0.3f));
          if (lr_recheck_.size() >= 64) {
            // Drop the oldest entry that is not a font atlas candidate (there are only a few).
            auto victim = std::find_if(lr_recheck_.begin(), lr_recheck_.end(),
                                       [](const LrRecheck& r) { return !r.font; });
            lr_recheck_.erase(victim != lr_recheck_.end() ? victim : lr_recheck_.begin());
            if (lr_recheck_next_ >= lr_recheck_.size()) lr_recheck_next_ = 0;
          }
          lr_recheck_.push_back({job.key, lr_frame_, font});
        }
      }
      continue;
    }
    if (!job.image) {
      REXLOG_WARN("LR textures: {} - {}", job.entry->path.string(), job.error);
      continue;
    }
    LrInstallReplacement(texture, job.result.hash, *job.entry, *job.image);
    texture.lr_composite_ = job.composite && texture.replacement_;
    if (texture.lr_composite_) {
      static uint32_t composite_logged = 0;
      if (composite_logged < 100) {
        ++composite_logged;
        REXLOG_INFO("LR textures: {:016X} {}x{} at {:08X} shows font atlas tiles: {}; {} blocks the game "
                    "changed rebuilt",
                    job.result.hash, job.width, job.height, uint32_t(job.key.base_page) << 12, job.font_desc,
                    job.font_dirty);
      }
    }
  }
}

void TextureCache::LrRetireReplacement(Texture& texture) {
  Texture* old = texture.replacement_.get();
  if (!old) {
    return;
  }
  // A binding made earlier in the frame may still point at the replacement; it is destroyed
  // once the GPU is done with it, so point those bindings back at the guest texture.
  // (LR_TEST_NO_REBIND=1 skips this, to reproduce the old behaviour in tests.)
  static const bool no_rebind = std::getenv("LR_TEST_NO_REBIND") != nullptr;
  for (size_t i = 0; i < texture_bindings_.size() && !no_rebind; ++i) {
    if (texture_bindings_[i].texture == old) {
      texture_bindings_[i].texture = &texture;
      lr_bindings_changed_ |= UINT32_C(1) << i;
    }
  }
  // The GPU may still be reading it.
  replacements_retired_.emplace_back(current_submission_index_, std::move(texture.replacement_));
}

void TextureCache::LrInstallReplacement(Texture& texture, uint64_t hash,
                                        const texture_replacement::Entry& entry,
                                        const texture_replacement::Image& image) {
  TextureKey key = texture.key();
  const std::filesystem::path& path = entry.path;
  if (image.width > 8192 || image.height > 8192) {
    REXLOG_WARN("LR textures: {} - larger than 8192", path.string());
    return;
  }
  TextureKey replacement_key = key;
  replacement_key.width_minus_1 = image.width - 1;
  replacement_key.height_minus_1 = image.height - 1;
  replacement_key.mip_max_level = uint32_t(std::min(image.levels.size(), size_t(16)) - 1);
  replacement_key.tiled = 0;
  replacement_key.packed_mips = 0;
  std::unique_ptr<Texture> replacement = CreateReplacementTexture(replacement_key, image);
  if (!replacement) {
    REXLOG_WARN("LR textures: {} - DXGI format {} can't replace guest format {} ({}x{})",
                path.string(), image.dxgi_format, FormatInfo::Get(key.format)->name,
                key.GetWidth(), key.GetHeight());
    return;
  }
  replacement->replacement_source_ = &texture;
  lr_copy_back_matched_.insert((uint64_t(key.base_page) << 44) | texture.GetGuestBaseSize());
  texture.replacement_ = std::move(replacement);
  texture.replacement_set_ = entry.set;
  static uint32_t logged = 0;
  if (logged < 200) {
    ++logged;
    REXLOG_INFO("LR textures: {:016X} {}x{} {} -> {} ({}x{}, {} mips)", hash, key.GetWidth(),
                key.GetHeight(), FormatInfo::Get(key.format)->name, path.filename().string(),
                image.width, image.height, image.levels.size());
  }
}

void TextureCache::DumpTextureBase(const Texture& texture, uint64_t hash,
                                   const uint8_t* guest_data) {
  TextureKey key = texture.key();
  uint32_t dxgi_format;
  switch (key.format) {
    case xenos::TextureFormat::k_DXT1:
      dxgi_format = texture_replacement::kDxgiBC1Unorm;
      break;
    case xenos::TextureFormat::k_DXT2_3:
      dxgi_format = texture_replacement::kDxgiBC2Unorm;
      break;
    case xenos::TextureFormat::k_DXT4_5:
      dxgi_format = texture_replacement::kDxgiBC3Unorm;
      break;
    case xenos::TextureFormat::k_DXT5A:
      dxgi_format = texture_replacement::kDxgiBC4Unorm;
      break;
    case xenos::TextureFormat::k_DXN:
      dxgi_format = texture_replacement::kDxgiBC5Unorm;
      break;
    case xenos::TextureFormat::k_8_8_8_8:
      dxgi_format = texture_replacement::kDxgiR8G8B8A8Unorm;
      break;
    default:
      return;
  }
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  uint32_t block_width = format_info->block_width, block_height = format_info->block_height;
  uint32_t bytes_per_block = format_info->bytes_per_block();
  uint32_t width = key.GetWidth(), height = key.GetHeight();
  uint32_t width_blocks = (width + block_width - 1) / block_width;
  uint32_t height_blocks = (height + block_height - 1) / block_height;
  const texture_util::TextureGuestLayout::Level& level = texture.guest_layout().base;
  uint32_t pitch_blocks = level.row_pitch_bytes / bytes_per_block;
  if (!pitch_blocks) {
    return;
  }
  std::vector<uint8_t> out(size_t(width_blocks) * height_blocks * bytes_per_block);
  xenos::Endian endian = key.endianness;
  if (key.tiled) {
    texture_conversion::UntileInfo untile = {};
    untile.width = width_blocks;
    untile.height = height_blocks;
    untile.input_pitch = pitch_blocks;
    untile.output_pitch = width_blocks;
    untile.input_format_info = format_info;
    untile.output_format_info = format_info;
    untile.copy_callback = [endian](void* output, const void* input, size_t length) {
      texture_conversion::CopySwapBlock(endian, output, input, length);
    };
    texture_conversion::Untile(out.data(), guest_data, &untile);
  } else {
    for (uint32_t y = 0; y < height_blocks; ++y) {
      texture_conversion::CopySwapBlock(endian, out.data() + size_t(y) * width_blocks * bytes_per_block,
                                        guest_data + size_t(y) * level.row_pitch_bytes,
                                        size_t(width_blocks) * bytes_per_block);
    }
  }
  std::filesystem::path path =
      texture_replacement::Registry::Get().root() / "dump" /
      fmt::format("{:016X}_{}x{}_{}.dds", hash, width, height, format_info->name);
  texture_replacement::SaveDds(path, width, height, dxgi_format, out.data(), out.size());
}

const char* TextureCache::TextureKey::GetLogDimensionName(xenos::DataDimension dimension) {
  switch (dimension) {
    case xenos::DataDimension::k1D:
      return "1D";
    case xenos::DataDimension::k2DOrStacked:
      return "2D";
    case xenos::DataDimension::k3D:
      return "3D";
    case xenos::DataDimension::kCube:
      return "cube";
    default:
      assert_unhandled_case(dimension);
      return "unknown";
  }
}

void TextureCache::TextureKey::LogAction(const char* action) const {
  REXGPU_TRACE(
      "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
      "base at 0x{:08X} (pitch {}), mips at 0x{:08X}",
      action, tiled ? "tiled" : "linear", scaled_resolve ? "scaled " : "", GetWidth(), GetHeight(),
      GetDepthOrArraySize(), GetLogDimensionName(), FormatInfo::Get(format)->name,
      mip_max_level + 1, packed_mips ? "" : "un", mip_max_level != 0 ? "s" : "", base_page << 12,
      pitch << 5, mip_page << 12);
}

void TextureCache::Texture::LogAction(const char* action) const {
  REXGPU_TRACE(
      "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
      "base at 0x{:08X} (pitch {}, size 0x{:08X}), mips at 0x{:08X} (size "
      "0x{:08X})",
      action, key_.tiled ? "tiled" : "linear", key_.scaled_resolve ? "scaled " : "",
      key_.GetWidth(), key_.GetHeight(), key_.GetDepthOrArraySize(), key_.GetLogDimensionName(),
      FormatInfo::Get(key_.format)->name, key_.mip_max_level + 1, key_.packed_mips ? "" : "un",
      key_.mip_max_level != 0 ? "s" : "", key_.base_page << 12, key_.pitch << 5, GetGuestBaseSize(),
      key_.mip_page << 12, GetGuestMipsSize());
}

// The texture must be in the recent usage list. Place it in front now because
// after creation, the texture will likely be used immediately, and it should
// not be destroyed immediately after creation if dropping of old textures is
// performed somehow. The list is maintained by the Texture, not the
// TextureCache itself (unlike the `textures_` container).
TextureCache::Texture::Texture(TextureCache& texture_cache, const TextureKey& key, bool track_usage)
    : texture_cache_(texture_cache),
      key_(key),
      guest_layout_(key.GetGuestLayout()),
      last_usage_submission_index_(texture_cache.current_submission_index_),
      last_usage_time_(texture_cache.current_submission_time_),
      used_previous_(track_usage ? texture_cache.texture_used_last_ : nullptr),
      used_next_(nullptr),
      in_usage_list_(track_usage) {
  if (track_usage) {
    if (texture_cache.texture_used_last_) {
      texture_cache.texture_used_last_->used_next_ = this;
    } else {
      texture_cache.texture_used_first_ = this;
    }
    texture_cache.texture_used_last_ = this;
  }

  // Never try to upload data that doesn't exist.
  base_outdated_ = guest_layout().base.level_data_extent_bytes != 0;
  mips_outdated_ = guest_layout().mips_total_extent_bytes != 0;
  outdated_mask_.store(
      (base_outdated_ ? kOutdatedBitBase : 0) | (mips_outdated_ ? kOutdatedBitMips : 0),
      std::memory_order_relaxed);
}

TextureCache::Texture::~Texture() {
  if (mips_watch_handle_) {
    texture_cache().shared_memory().UnwatchMemoryRange(mips_watch_handle_);
  }
  if (base_watch_handle_) {
    texture_cache().shared_memory().UnwatchMemoryRange(base_watch_handle_);
  }

  if (in_usage_list_) {
    if (used_previous_) {
      used_previous_->used_next_ = used_next_;
    } else {
      texture_cache_.texture_used_first_ = used_next_;
    }
    if (used_next_) {
      used_next_->used_previous_ = used_previous_;
    } else {
      texture_cache_.texture_used_last_ = used_previous_;
    }
  }

  texture_cache_.UpdateTexturesTotalHostMemoryUsage(0, host_memory_usage_);
}

void TextureCache::Texture::MakeUpToDateAndWatch(
    const std::unique_lock<std::recursive_mutex>& global_lock) {
  SharedMemory& shared_memory = texture_cache().shared_memory();
  if (base_outdated_) {
    assert_not_zero(GetGuestBaseSize());
    base_outdated_ = false;
    base_watch_handle_ = shared_memory.WatchMemoryRange(
        key().base_page << 12, GetGuestBaseSize(), TextureCache::WatchCallback, this, nullptr, 0);
    outdated_mask_.fetch_and(~kOutdatedBitBase, std::memory_order_release);
  }
  if (mips_outdated_) {
    assert_not_zero(GetGuestMipsSize());
    mips_outdated_ = false;
    mips_watch_handle_ = shared_memory.WatchMemoryRange(
        key().mip_page << 12, GetGuestMipsSize(), TextureCache::WatchCallback, this, nullptr, 1);
    outdated_mask_.fetch_and(~kOutdatedBitMips, std::memory_order_release);
  }
}

void TextureCache::Texture::MarkAsUsed() {
  if (replacement_source_) {
    replacement_source_->MarkAsUsed();
    return;
  }
  if (!in_usage_list_) {
    return;
  }
  assert_true(last_usage_submission_index_ <= texture_cache_.current_submission_index_);
  // This is called very frequently, don't relink unless needed for caching.
  if (last_usage_submission_index_ >= texture_cache_.current_submission_index_) {
    return;
  }
  last_usage_submission_index_ = texture_cache_.current_submission_index_;
  last_usage_time_ = texture_cache_.current_submission_time_;
  if (used_next_ == nullptr) {
    // Already the most recently used.
    return;
  }
  if (used_previous_ != nullptr) {
    used_previous_->used_next_ = used_next_;
  } else {
    texture_cache_.texture_used_first_ = used_next_;
  }
  used_next_->used_previous_ = used_previous_;
  used_previous_ = texture_cache_.texture_used_last_;
  used_next_ = nullptr;
  texture_cache_.texture_used_last_->used_next_ = this;
  texture_cache_.texture_used_last_ = this;
}

void TextureCache::Texture::WatchCallback(
    [[maybe_unused]] const std::unique_lock<std::recursive_mutex>& global_lock, bool is_mip) {
  if (is_mip) {
    assert_not_zero(GetGuestMipsSize());
    mips_outdated_ = true;
    mips_watch_handle_ = nullptr;
    outdated_mask_.fetch_or(kOutdatedBitMips, std::memory_order_release);
  } else {
    assert_not_zero(GetGuestBaseSize());
    base_outdated_ = true;
    base_watch_handle_ = nullptr;
    outdated_mask_.fetch_or(kOutdatedBitBase, std::memory_order_release);
  }
}

void TextureCache::WatchCallback(const std::unique_lock<std::recursive_mutex>& global_lock,
                                 void* context, void* data, uint64_t argument,
                                 bool invalidated_by_gpu) {
  Texture& texture = *static_cast<Texture*>(context);
  (invalidated_by_gpu ? lr::frame_stats().invalidated_by_gpu : lr::frame_stats().invalidated_by_cpu)
      .fetch_add(1, std::memory_order_relaxed);
  texture.WatchCallback(global_lock, argument != 0);
  texture.texture_cache().texture_became_outdated_.store(true, std::memory_order_release);
}

void TextureCache::DestroyAllTextures(bool from_destructor) {
  ResetTextureBindings(from_destructor);
  textures_.clear();
  replacements_retired_.clear();
  COUNT_profile_set("gpu/texture_cache/textures", 0);
}

TextureCache::Texture* TextureCache::FindOrCreateTexture(TextureKey key) {
  // Check if the texture is a scaled resolve texture.
  if (IsDrawResolutionScaled() && key.tiled && IsScaledResolveSupportedForFormat(key)) {
    texture_util::TextureGuestLayout scaled_resolve_guest_layout = key.GetGuestLayout();
    if ((scaled_resolve_guest_layout.base.level_data_extent_bytes &&
         IsRangeScaledResolved(key.base_page << 12,
                               scaled_resolve_guest_layout.base.level_data_extent_bytes)) ||
        (scaled_resolve_guest_layout.mips_total_extent_bytes &&
         IsRangeScaledResolved(key.mip_page << 12,
                               scaled_resolve_guest_layout.mips_total_extent_bytes))) {
      key.scaled_resolve = 1;
    }
  }

  auto get_host_extent = [this, &key](uint32_t& host_width_out, uint32_t& host_height_out,
                                      uint32_t& host_depth_or_array_size_out) {
    host_width_out = key.GetWidth();
    host_height_out = key.GetHeight();
    if (key.scaled_resolve) {
      host_width_out *= draw_resolution_scale_x();
      host_height_out *= draw_resolution_scale_y();
    }
    host_depth_or_array_size_out = key.GetDepthOrArraySize();
  };
  auto try_drop_top_level = [&key]() {
    if (!key.mip_page || !key.mip_max_level) {
      return false;
    }
    uint32_t old_width = key.GetWidth();
    uint32_t old_height = key.GetHeight();
    uint32_t old_depth_or_array_size = key.GetDepthOrArraySize();
    uint32_t new_width = std::max(old_width >> 1, UINT32_C(1));
    uint32_t new_height = std::max(old_height >> 1, UINT32_C(1));
    uint32_t new_depth_or_array_size = key.dimension == xenos::DataDimension::k3D
                                           ? std::max(old_depth_or_array_size >> 1, UINT32_C(1))
                                           : old_depth_or_array_size;
    if (new_width == old_width && new_height == old_height &&
        new_depth_or_array_size == old_depth_or_array_size) {
      return false;
    }
    const FormatInfo* format_info = FormatInfo::Get(key.format);
    uint32_t rebased_pitch_texels = std::max(rex::next_pow2(old_width) >> 1, UINT32_C(1));
    rebased_pitch_texels = rex::align(rebased_pitch_texels, format_info->block_width);
    rebased_pitch_texels = rex::align(rebased_pitch_texels, xenos::kTextureTileWidthHeight);
    if (!key.tiled) {
      uint32_t rebased_pitch_blocks = rebased_pitch_texels / format_info->block_width;
      uint32_t rebased_row_pitch_bytes = rebased_pitch_blocks * format_info->bytes_per_block();
      rebased_row_pitch_bytes =
          rex::align(rebased_row_pitch_bytes, xenos::kTextureLinearRowAlignmentBytes);
      rebased_pitch_blocks = rebased_row_pitch_bytes / format_info->bytes_per_block();
      rebased_pitch_texels = rebased_pitch_blocks * format_info->block_width;
    }
    uint32_t rebased_pitch = std::max(rebased_pitch_texels >> 5, UINT32_C(1));
    if (rebased_pitch > ((UINT32_C(1) << 9) - 1)) {
      return false;
    }
    key.width_minus_1 = new_width - 1;
    key.height_minus_1 = new_height - 1;
    key.depth_or_array_size_minus_1 = new_depth_or_array_size - 1;
    key.base_page = key.mip_page;
    key.mip_page = 0;
    key.pitch = rebased_pitch;
    key.mip_max_level = 0;
    key.packed_mips = 0;
    return true;
  };
  uint32_t host_width, host_height, host_depth_or_array_size;
  get_host_extent(host_width, host_height, host_depth_or_array_size);
  // If the host can't support the full texture extent, first try using the
  // unscaled version of a scaled resolve texture, then fall back to the first
  // stored mip level only.
  uint32_t max_host_width_height = GetMaxHostTextureWidthHeight(key.dimension);
  uint32_t max_host_depth_or_array_size = GetMaxHostTextureDepthOrArraySize(key.dimension);
  while (true) {
    bool width_height_too_large =
        host_width > max_host_width_height || host_height > max_host_width_height;
    bool depth_or_array_too_large = host_depth_or_array_size > max_host_depth_or_array_size;
    if (!width_height_too_large && !depth_or_array_too_large) {
      break;
    }
    bool size_adjusted = false;
    if (key.scaled_resolve) {
      key.scaled_resolve = 0;
      size_adjusted = true;
    } else if ((width_height_too_large ||
                (depth_or_array_too_large && key.dimension == xenos::DataDimension::k3D)) &&
               try_drop_top_level()) {
      size_adjusted = true;
    }
    if (!size_adjusted) {
      return nullptr;
    }
    get_host_extent(host_width, host_height, host_depth_or_array_size);
  }

  // Try to find an existing texture.
  // TODO(Triang3l): Reuse a texture with mip_page unchanged, but base_page
  // previously 0, now not 0, to save memory - common case in streaming.
  auto found_texture_it = textures_.find(key);
  if (found_texture_it != textures_.end()) {
    return found_texture_it->second.get();
  }

  // Create the texture and add it to the map.
  Texture* texture;
  {
    std::unique_ptr<Texture> new_texture = CreateTexture(key);
    if (!new_texture) {
      key.LogAction("Failed to create");
      return nullptr;
    }
    assert_true(new_texture->key() == key);
    texture = textures_.emplace(key, std::move(new_texture)).first->second.get();
  }
  COUNT_profile_set("gpu/texture_cache/textures", textures_.size());
  texture->LogAction("Created");
  return texture;
}

bool TextureCache::LoadTextureData(Texture& texture) {
  PendingTextureLoad pending_load;
  PendingSharedMemoryRange pending_ranges[2];
  size_t pending_range_count = 0;
  if (!PrepareTextureLoad(texture, pending_load, pending_ranges, pending_range_count)) {
    return true;
  }

  std::pair<uint32_t, uint32_t> pending_range_pairs[2];
  for (size_t i = 0; i < pending_range_count; ++i) {
    pending_range_pairs[i] = std::make_pair(pending_ranges[i].start, pending_ranges[i].length);
  }
  if (!shared_memory().RequestRanges(pending_range_pairs, pending_range_count)) {
    return false;
  }

  return CommitPreparedTextureLoad(pending_load);
}

void TextureCache::BindingInfoFromFetchConstant(const xenos::xe_gpu_texture_fetch_t& fetch,
                                                TextureKey& key_out, uint8_t* swizzled_signs_out) {
  // Reset the key and the signedness.
  key_out.MakeInvalid();
  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out = uint8_t(xenos::TextureSign::kUnsigned) * uint8_t(0b01010101);
  }

  switch (fetch.type) {
    case xenos::FetchConstantType::kTexture:
      break;
    case xenos::FetchConstantType::kInvalidTexture:
      if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
        break;
      }
      REXGPU_WARN(
          "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
          "has \"invalid\" type! This is incorrect behavior, but you can try "
          "bypassing this by launching Xenia with "
          "--gpu_allow_invalid_fetch_constants=true.",
          fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
      return;
    default:
      REXGPU_WARN(
          "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
          "is completely invalid!",
          fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
      return;
  }

  uint32_t width_minus_1, height_minus_1, depth_or_array_size_minus_1;
  uint32_t base_page, mip_page, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_or_array_size_minus_1, &base_page,
                                                 &mip_page, nullptr, &mip_max_level);
  if (base_page == 0 && mip_page == 0) {
    // No texture data at all.
    return;
  }
  if (fetch.dimension == xenos::DataDimension::k1D) {
    bool is_invalid_1d = false;
    // TODO(Triang3l): Support long 1D textures.
    if (width_minus_1 >= xenos::kTexture2DCubeMaxWidthHeight) {
      REXGPU_ERROR(
          "1D texture is too wide ({}) - ignoring! Report the game to Xenia "
          "developers",
          width_minus_1 + 1);
      is_invalid_1d = true;
    }
    assert_false(fetch.tiled);
    if (fetch.tiled) {
      REXGPU_ERROR(
          "1D texture has tiling enabled in the fetch constant, but this "
          "appears to be completely wrong - ignoring! Report the game to Xenia "
          "developers");
      is_invalid_1d = true;
    }
    assert_false(fetch.packed_mips);
    if (fetch.packed_mips) {
      REXGPU_ERROR(
          "1D texture has packed mips enabled in the fetch constant, but this "
          "appears to be completely wrong - ignoring! Report the game to Xenia "
          "developers");
      is_invalid_1d = true;
    }
    if (is_invalid_1d) {
      return;
    }
  }

  xenos::TextureFormat format = GetBaseFormat(fetch.format);

  key_out.base_page = base_page;
  key_out.mip_page = mip_page;
  key_out.dimension = fetch.dimension;
  key_out.width_minus_1 = width_minus_1;
  key_out.height_minus_1 = height_minus_1;
  key_out.depth_or_array_size_minus_1 = depth_or_array_size_minus_1;
  key_out.pitch = fetch.pitch;
  key_out.mip_max_level = mip_max_level;
  key_out.tiled = fetch.tiled;
  key_out.packed_mips = fetch.packed_mips;
  key_out.format = format;
  key_out.endianness = fetch.endianness;

  key_out.is_valid = 1;

  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out = texture_util::SwizzleSigns(fetch);
  }
}

void TextureCache::ResetTextureBindings(bool from_destructor) {
  uint32_t bindings_reset = 0;
  for (size_t i = 0; i < texture_bindings_.size(); ++i) {
    TextureBinding& binding = texture_bindings_[i];
    if (!binding.key.is_valid) {
      continue;
    }
    binding.Reset();
    bindings_reset |= UINT32_C(1) << i;
  }
  texture_bindings_in_sync_ &= ~bindings_reset;
  if (!from_destructor && bindings_reset) {
    UpdateTextureBindingsImpl(bindings_reset);
  }
}

void TextureCache::UpdateTexturesTotalHostMemoryUsage(uint64_t add, uint64_t subtract) {
  textures_total_host_memory_usage_ = textures_total_host_memory_usage_ - subtract + add;
  COUNT_profile_set(
      "gpu/texture_cache/total_host_memory_usage_mb",
      uint32_t((textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20));
}

bool TextureCache::IsRangeScaledResolved(uint32_t start_unscaled, uint32_t length_unscaled) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }

  start_unscaled = std::min(start_unscaled, SharedMemory::kBufferSize);
  length_unscaled = std::min(length_unscaled, SharedMemory::kBufferSize - start_unscaled);
  if (!length_unscaled) {
    return false;
  }

  // Two-level check for faster rejection since resolve targets are usually
  // placed in relatively small and localized memory portions (confirmed by
  // testing - pretty much all times the deeper level was entered, the texture
  // was a resolve target).
  uint32_t page_first = start_unscaled >> 12;
  uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
  uint32_t block_first = page_first >> 5;
  uint32_t block_last = page_last >> 5;
  uint32_t l2_block_first = block_first >> 6;
  uint32_t l2_block_last = block_last >> 6;
  LR_GLOBAL_LOCK();
  for (uint32_t i = l2_block_first; i <= l2_block_last; ++i) {
    uint64_t l2_block = scaled_resolve_pages_l2_[i];
    if (i == l2_block_first) {
      l2_block &= ~((UINT64_C(1) << (block_first & 63)) - 1);
    }
    if (i == l2_block_last && (block_last & 63) != 63) {
      l2_block &= (UINT64_C(1) << ((block_last & 63) + 1)) - 1;
    }
    uint32_t block_relative_index;
    while (rex::bit_scan_forward(l2_block, &block_relative_index)) {
      l2_block &= ~(UINT64_C(1) << block_relative_index);
      uint32_t block_index = (i << 6) + block_relative_index;
      uint32_t check_bits = UINT32_MAX;
      if (block_index == block_first) {
        check_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (block_index == block_last && (page_last & 31) != 31) {
        check_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      if (scaled_resolve_pages_[block_index] & check_bits) {
        return true;
      }
    }
  }
  return false;
}

void TextureCache::ScaledResolveGlobalWatchCallbackThunk(
    const std::unique_lock<std::recursive_mutex>& global_lock, void* context,
    uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu) {
  TextureCache* texture_cache = reinterpret_cast<TextureCache*>(context);
  texture_cache->ScaledResolveGlobalWatchCallback(global_lock, address_first, address_last,
                                                  invalidated_by_gpu);
}

void TextureCache::ScaledResolveGlobalWatchCallback(
    const std::unique_lock<std::recursive_mutex>& global_lock, uint32_t address_first,
    uint32_t address_last, bool invalidated_by_gpu) {
  assert_true(IsDrawResolutionScaled());
  if (invalidated_by_gpu) {
    // Resolves themselves do exactly the opposite of what this should do.
    return;
  }
  // Mark scaled resolve ranges as non-scaled. Textures themselves will be
  // invalidated by their shared memory watches.
  uint32_t resolve_page_first = address_first >> 12;
  uint32_t resolve_page_last = address_last >> 12;
  uint32_t resolve_block_first = resolve_page_first >> 5;
  uint32_t resolve_block_last = resolve_page_last >> 5;
  uint32_t resolve_l2_block_first = resolve_block_first >> 6;
  uint32_t resolve_l2_block_last = resolve_block_last >> 6;
  for (uint32_t i = resolve_l2_block_first; i <= resolve_l2_block_last; ++i) {
    uint64_t resolve_l2_block = scaled_resolve_pages_l2_[i];
    if (REXCVAR_GET(pre_mask_resolve_l2_block)) {
      // Pre-mask to only process blocks within the write range.
      if (i == resolve_l2_block_first) {
        resolve_l2_block &= ~((UINT64_C(1) << (resolve_block_first & 63)) - 1);
      }
      if (i == resolve_l2_block_last && (resolve_block_last & 63) != 63) {
        resolve_l2_block &= (UINT64_C(1) << ((resolve_block_last & 63) + 1)) - 1;
      }
    }
    uint32_t resolve_block_relative_index;
    while (rex::bit_scan_forward(resolve_l2_block, &resolve_block_relative_index)) {
      resolve_l2_block &= ~(UINT64_C(1) << resolve_block_relative_index);
      uint32_t resolve_block_index = (i << 6) + resolve_block_relative_index;
      uint32_t resolve_keep_bits = 0;
      if (resolve_block_index == resolve_block_first) {
        resolve_keep_bits |= (UINT32_C(1) << (resolve_page_first & 31)) - 1;
      }
      if (resolve_block_index == resolve_block_last && (resolve_page_last & 31) != 31) {
        resolve_keep_bits |= ~((UINT32_C(1) << ((resolve_page_last & 31) + 1)) - 1);
      }
      scaled_resolve_pages_[resolve_block_index] &= resolve_keep_bits;
      if (scaled_resolve_pages_[resolve_block_index] == 0) {
        scaled_resolve_pages_l2_[i] &= ~(UINT64_C(1) << resolve_block_relative_index);
      }
    }
  }
}

}  // namespace rex::graphics
