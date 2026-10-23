#pragma once
// Lightning Returns port: per-frame work counters for the "LR hitch" log line (frames
// over 50 ms list what the renderer did during them).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace rex::graphics::lr {
struct FrameStats {
  std::atomic<uint64_t> upload_bytes{0};      // guest memory copied to the GPU buffer
  std::atomic<uint32_t> textures{0};          // texture (re)loads
  std::atomic<uint64_t> texture_bytes{0};
  std::atomic<uint32_t> shaders{0};           // Xenos -> DXBC translations
  std::atomic<uint64_t> shader_us{0};
  std::atomic<uint32_t> pipelines{0};         // D3D12 pipeline creations (any thread)
  std::atomic<uint64_t> pipeline_us{0};
  std::atomic<uint32_t> evicted{0};           // textures destroyed by the memory limit
  std::atomic<uint64_t> texture_us{0};        // renderer thread time requesting / loading textures
  std::atomic<uint64_t> replace_us{0};        // of which texture replacement bookkeeping
  // Where the GPU thread's time goes, summed over the "LR frames" 10 s window.
  std::atomic<uint64_t> idle_us{0};           // ring empty: waiting for the game's commands
  std::atomic<uint64_t> regwait_us{0};        // WAIT_REG_MEM: waiting for a value the game writes
  std::atomic<uint32_t> regwaits{0};
  std::atomic<uint64_t> fence_us{0};          // waiting for the host GPU to finish submissions
  // CPU writes to memory the renderer watches (each one is a page fault + invalidation).
  std::atomic<uint32_t> cpu_invalidations{0};
  std::atomic<uint64_t> cpu_invalidated_bytes{0};
  // The same waits per frame, and what the GPU thread executed, for the "LR hitch" line.
  std::atomic<uint64_t> frame_idle_us{0};
  std::atomic<uint64_t> frame_regwait_us{0};
  std::atomic<uint64_t> frame_fence_us{0};
  std::atomic<uint32_t> draws{0};             // draw packets given to the backend
  std::atomic<uint64_t> draw_us{0};           // including resolves (copy draws)
  std::atomic<uint32_t> resolves{0};          // EDRAM -> memory copies
  std::atomic<uint64_t> resolve_us{0};
  std::atomic<uint64_t> swap_us{0};           // handing the frame to the presenter
  // Shader memexport writes copied back to guest memory (fonts / UI sheets the game moves on
  // the GPU), and the times the renderer had to wait for the GPU to do it.
  std::atomic<uint32_t> memexports{0};
  std::atomic<uint64_t> memexport_bytes{0};
  // Textures thrown out because memory under them changed: by GPU writes (memexport,
  // resolves) or by CPU writes.
  std::atomic<uint32_t> invalidated_by_gpu{0};
  std::atomic<uint32_t> invalidated_by_cpu{0};
  std::atomic<uint32_t> readbacks{0};          // ranges copied back for texture replacement
  std::atomic<uint64_t> readback_bytes{0};
  std::atomic<uint32_t> readback_syncs{0};
  std::atomic<uint64_t> readback_sync_us{0};
  // The runtime's global lock as the renderer code takes it (shared memory and texture cache
  // bookkeeping, also run by the page-fault handler on game threads): longest wait to get it
  // and longest hold, with the function, over the 10 s window.
  std::atomic<uint64_t> lock_wait_max_us{0};
  std::atomic<const char*> lock_wait_site{nullptr};
  std::atomic<uint64_t> lock_hold_max_us{0};
  std::atomic<const char*> lock_hold_site{nullptr};
  std::atomic<uint32_t> lock_holds_over_2ms{0};
  std::atomic<uint64_t> lock_hold_us{0};
};
inline uint64_t now_us() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
// Adds the time until the end of the scope to a counter.
struct ScopedTime {
  std::atomic<uint64_t>& counter;
  uint64_t start;
  explicit ScopedTime(std::atomic<uint64_t>& c) : counter(c), start(now_us()) {}
  ~ScopedTime() { counter.fetch_add(now_us() - start, std::memory_order_relaxed); }
};
inline FrameStats& frame_stats() {
  static FrameStats stats;
  return stats;
}
inline void raise_max(std::atomic<uint64_t>& max, std::atomic<const char*>& site, uint64_t value,
                      const char* where) {
  uint64_t prev = max.load(std::memory_order_relaxed);
  while (value > prev) {
    if (max.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
      site.store(where, std::memory_order_relaxed);
      break;
    }
  }
}
// Records the wait for and the hold of the global lock taken just before it (LR_GLOBAL_LOCK).
struct LockHold {
  const char* site;
  uint64_t acquired;
  LockHold(const char* where, uint64_t requested) : site(where), acquired(now_us()) {
    raise_max(frame_stats().lock_wait_max_us, frame_stats().lock_wait_site, acquired - requested, site);
  }
  ~LockHold() { note_hold(site, now_us() - acquired); }
  static void note_hold(const char* where, uint64_t held) {
    FrameStats& s = frame_stats();
    raise_max(s.lock_hold_max_us, s.lock_hold_site, held, where);
    if (held > 2000) s.lock_holds_over_2ms.fetch_add(1, std::memory_order_relaxed);
    if (held > 200) s.lock_hold_us.fetch_add(held, std::memory_order_relaxed);
  }
};
// Settings > Resolution while the game runs: the exe asks for a draw resolution scale through
// the plugin's lr_set_resolution_scale export; the renderer thread rebuilds its scaled state
// at the next frame boundary (CommandProcessor::LrApplyResolutionScale), after which the
// texture and render target caches use resolution_scale_override instead of the setting.
inline std::atomic<int32_t>& requested_resolution_scale() {
  static std::atomic<int32_t> scale{0};
  return scale;
}
inline std::atomic<int32_t>& resolution_scale_override() {
  static std::atomic<int32_t> scale{0};
  return scale;
}
// The scale the renderer's context was last set up with (lr_get_resolution_scale), 0 = none yet.
inline std::atomic<int32_t>& active_resolution_scale() {
  static std::atomic<int32_t> scale{0};
  return scale;
}
// Post-effect trace (LR_FX_TRACE=1; the Android build sets it), for the battle-exit grey filter
// flicker on the phone: every frame that crossfades by the blend constant, and the 90 frames
// after one, logged as one "LR fx" line per frame (time since the last frame, front buffer,
// draws, and the first crossfade draw with its constant) plus that frame's resolves and texture
// loads in order; at most 5000 lines. GPU command thread only.
struct FxTrace {
  bool on = [] {
    const char* v = std::getenv("LR_FX_TRACE");
    return v && v[0] == '1';
  }();
  uint32_t frame = 0;
  uint32_t draw = 0;          // draws so far in this frame
  uint32_t const_draws = 0;   // of which crossfades by the blend constant
  std::string blend;          // the first of those: draw, blend control, constant
  std::vector<std::string> events;
  int tail = 0;
  uint64_t last_swap_us = 0;
  uint32_t lines = 0;         // lines logged, capped
  // Mid-fade capture (Vulkan): the first time a crossfade's constant is near 0.5, the next two
  // frames are recorded (see LrCaptureFrameBoundary), keeping only the crossfade draws' textures.
  int capture_frames = 0;     // frames still to record
  bool capture_done = false;
  bool crossfade_draw = false;  // the current draw crossfades by the blend constant
};
// Colour source factor CONSTANT_COLOR, destination ONE_MINUS_CONSTANT_COLOR.
inline bool is_crossfade_blend(uint32_t blend_control) {
  return (blend_control & 0x1F) == 12 && ((blend_control >> 8) & 0x1F) == 13;
}
inline FxTrace& fx_trace() {
  static FxTrace trace;
  return trace;
}
inline void fx_event(std::string text) {
  FxTrace& t = fx_trace();
  if (t.on && t.events.size() < 160) t.events.push_back(std::to_string(t.draw) + ":" + text);
}
}  // namespace rex::graphics::lr

// auto global_lock = global_critical_region_.Acquire(), timed.
#define LR_GLOBAL_LOCK()                                                   \
  const uint64_t lr_lock_requested_ = ::rex::graphics::lr::now_us();       \
  auto global_lock = global_critical_region_.Acquire();                    \
  ::rex::graphics::lr::LockHold lr_lock_hold_(__func__, lr_lock_requested_)
