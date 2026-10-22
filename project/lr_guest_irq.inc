// LR_GUEST_IRQ_LOCK v7 (inserted by the Lightning Returns build script after codegen)
//
// ReXGlue turns mfmsr / mtmsrd r13,1 / mtmsrd rX,1 (guest "interrupts off/on") into its
// process-wide recursive mutex, which every guest thread then contends for many thousands
// of times per frame. On the console that state is per hardware thread and never blocks
// other cores. Two modes, chosen once at startup with LR_LOCK_MODE:
//   thread (default)  only a per-thread depth is kept; mfmsr reports it as MSR[EE].
//   global            ReXGlue's original mutex.
// In both modes a thread that re-enters "interrupts off" 64 times in a row within ~1000
// TSC ticks of its own previous exit is spinning on a game lock. If it holds levels of the
// mutex - including the one FunctionDispatcher::ExecuteInterrupt holds around the graphics
// interrupt handler (counted by lr_hooks.cpp) - it hands them all back, yields, and takes
// them back. Without that the handler spins on a lock whose owner waits for the mutex in a
// kernel call (movie/loading/title freezes). A spinning thread that holds nothing keeps
// spinning without yielding (v4 yielded there too, ~44k times/s in thread mode);
// LR_SPIN_YIELD=1 restores that for comparison.
// Every 10 s the mutex traffic is logged ("LR lock stats") to measure contention.

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
inline uint64_t lr_lock_ticks() { return __rdtsc(); }
#else
inline uint64_t lr_lock_ticks() {
  return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}
#endif
#include <chrono>
#include <cstdlib>
#include <thread>

struct LrLockSpin {
  uint64_t last_leave = 0;
  uint32_t rapid = 0;
  uint32_t guest_depth = 0;  // guest "interrupts off" levels on this thread
  uint32_t host_holds = 0;   // mutex levels the runtime holds for the guest (interrupt dispatch)
};

inline LrLockSpin& lr_lock_spin() {
  thread_local LrLockSpin spin;
  return spin;
}

inline bool lr_lock_global_mode() {
  static const bool global = [] {
    const char* mode = std::getenv("LR_LOCK_MODE");
    bool g = mode && (mode[0] == 'g' || mode[0] == 'G');
    REXLOG_INFO("LR lock mode: {}", g ? "global (ReXGlue mutex)" : "thread (per-thread state)");
    return g;
  }();
  return global;
}

inline bool lr_spin_yield_always() {
  static const bool always = [] {
    const char* v = std::getenv("LR_SPIN_YIELD");
    bool a = v && v[0] == '1';
    if (a) REXLOG_INFO("LR lock: spinning threads yield even when holding nothing (LR_SPIN_YIELD=1)");
    return a;
  }();
  return always;
}

struct LrLockStats {
  std::atomic<uint64_t> enters{0}, contended{0}, wait_ticks{0}, spin_yields{0}, spin_free{0};
  std::atomic<uint64_t> next_report{0};
};

inline LrLockStats& lr_lock_stats() {
  static LrLockStats stats;
  return stats;
}

inline void lr_lock_report(uint64_t now) {
  auto& s = lr_lock_stats();
  uint64_t due = s.next_report.load(std::memory_order_relaxed);
  if (now < due) return;
  constexpr uint64_t kInterval = 30'000'000'000ull;  // ~10 s at 3 GHz
  if (!s.next_report.compare_exchange_strong(due, now + kInterval)) return;
  if (due == 0) return;  // first call only arms the timer
  REXLOG_INFO("LR lock stats (10 s): enters={} contended={} wait_ms~={} spin_yields={} spin_no_yield={}",
              s.enters.exchange(0), s.contended.exchange(0),
              s.wait_ticks.exchange(0) / 3'000'000, s.spin_yields.exchange(0), s.spin_free.exchange(0));
}

inline void lr_lock_backoff() {
  auto& spin = lr_lock_spin();
  uint64_t now = lr_lock_ticks();
  if (now - spin.last_leave > 1000) {
    spin.rapid = 0;
    return;
  }
  if (++spin.rapid < 64) return;
  uint32_t held = spin.host_holds + (lr_lock_global_mode() ? spin.guest_depth : 0);
  if (held == 0 && !lr_spin_yield_always()) {
    // Nothing of ours blocks the lock owner; giving up the time slice only slows this thread.
    if ((spin.rapid & 63) == 0) lr_lock_stats().spin_free.fetch_add(64, std::memory_order_relaxed);
    return;
  }
  lr_lock_stats().spin_yields.fetch_add(1, std::memory_order_relaxed);
  auto& global = rex::thread::global_critical_region::mutex();
  for (uint32_t i = 0; i < held; ++i) global.unlock();
  if (spin.rapid < 16384) {
    std::this_thread::yield();
  } else {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  for (uint32_t i = 0; i < held; ++i) global.lock();
}

inline u64 lr_lock_check() {
  if (!lr_lock_global_mode()) return lr_lock_spin().guest_depth ? 0 : 0x8000;
  auto lock_ = rex::thread::global_critical_region::AcquireDirect();
  return ppc_global_lock_count_().load() ? 0 : 0x8000;
}

inline void lr_lock_enter() {
  lr_lock_backoff();
  auto& stats = lr_lock_stats();
  stats.enters.fetch_add(1, std::memory_order_relaxed);
  if (lr_lock_global_mode()) {
    auto& global = rex::thread::global_critical_region::mutex();
    if (!global.try_lock()) {
      uint64_t start = lr_lock_ticks();
      global.lock();
      stats.contended.fetch_add(1, std::memory_order_relaxed);
      stats.wait_ticks.fetch_add(lr_lock_ticks() - start, std::memory_order_relaxed);
    }
    ppc_global_lock_count_().fetch_add(1);
  }
  ++lr_lock_spin().guest_depth;
}

inline void lr_lock_leave() {
  auto& spin = lr_lock_spin();
  if (spin.guest_depth) --spin.guest_depth;
  if (lr_lock_global_mode()) {
    auto old_count_ = ppc_global_lock_count_().fetch_sub(1);
    assert(old_count_ >= 1 && "LeaveGlobalLock called without matching EnterGlobalLock");
    rex::thread::global_critical_region::mutex().unlock();
  }
  spin.last_leave = lr_lock_ticks();
  lr_lock_report(spin.last_leave);
}

// LR_FLUSH_NEVER: the generated code switches the host MXCSR flush-to-zero mode off before
// scalar FPU instructions and on before VMX ones (the PPC FPU keeps denormals, VMX flushes
// them). Each switch is an ldmxcsr, which serializes the pipeline; game loops that mix
// scalar and vector math paid it on every call (a third of the main thread's time in the
// Yusnaan profile). Keep flush mode off throughout instead. Scalar code must keep
// denormals exactly: the game moves 32-bit data (colors, alpha) through lfs/stfs, and
// flushing those bit patterns made the menus and loading screen invisible (step 10).
// VMX arithmetic now keeps denormal results instead of flushing them to zero.
#define enableFlushMode disableFlushMode

#define REX_CHECK_GLOBAL_LOCK() (lr_lock_check())
#define REX_ENTER_GLOBAL_LOCK() \
  do {                          \
    lr_lock_enter();            \
  } while (0)
#define REX_LEAVE_GLOBAL_LOCK() \
  do {                          \
    lr_lock_leave();            \
  } while (0)
