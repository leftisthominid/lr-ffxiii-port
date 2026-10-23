// Lightning Returns - Android declarations, force-included (-include) into the game sources of
// the Android build only (android/game/CMakeLists.txt). The few `#if defined(__ANDROID__)`
// lines in the shared sources call these; PC builds never see this header.
#pragma once

#if defined(__ANDROID__) && defined(__cplusplus)
#include <atomic>
#include <type_traits>
#include <version>

// The NDK r28 libc++ (LLVM 19 snapshot) has no std::atomic_ref yet. Minimal stand-in for the
// integral uses in the game sources (lr_hooks.cpp), on the compiler's __atomic builtins.
#if !defined(__cpp_lib_atomic_ref)
namespace std {
template <class T>
struct atomic_ref {
  static_assert(std::is_integral_v<T>, "lr_android.h atomic_ref: integral types only");
  explicit atomic_ref(T& ref) noexcept : ptr_(&ref) {}
  T load(memory_order o = memory_order_seq_cst) const noexcept { return __atomic_load_n(ptr_, int(o)); }
  void store(T v, memory_order o = memory_order_seq_cst) const noexcept { __atomic_store_n(ptr_, v, int(o)); }
  T exchange(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_exchange_n(ptr_, v, int(o)); }
  bool compare_exchange_strong(T& expected, T desired, memory_order o = memory_order_seq_cst) const noexcept {
    return __atomic_compare_exchange_n(ptr_, &expected, desired, false, int(o), int(memory_order_seq_cst));
  }
  T fetch_add(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_add(ptr_, v, int(o)); }
  T fetch_sub(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_sub(ptr_, v, int(o)); }
  T fetch_and(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_and(ptr_, v, int(o)); }
  T fetch_or(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_or(ptr_, v, int(o)); }
  T fetch_xor(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_xor(ptr_, v, int(o)); }
  operator T() const noexcept { return load(); }

 private:
  T* ptr_;
};
}  // namespace std
#endif

// Restarts the game the Android way (the PC build starts a new copy of its executable): asks
// the activity to relaunch the app in a fresh process. Returns true when the relaunch is
// scheduled; the caller then ends this process.
bool LrAndroidRestartProcess(int generation);

// Called by the game's main thread once per frame with that frame's work time (without the
// frame-pacing sleep) and the frame rate the game runs at. Reports it to Android's performance
// hint API (ADPF), which raises CPU clocks and moves the threads to fast cores when frames run
// long; the session covers the main thread, the GPU command thread, and the busiest game threads.
void LrAndroidFrameDone(uint64_t work_us, double frame_rate);

// Touch layer (lr_hooks.cpp; driven from Java through lr_android_main.cpp's JNI functions).
// The on-screen pad's state, merged into controller 1 (XInput layout: buttons, triggers 0-255,
// sticks -32768..32767, Y up).
void LrTouchSetPad(bool active, uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly,
                   int16_t rx, int16_t ry);
// While the Ark menu is open the game sees no controller buttons; presses are collected for the
// menu (LrTouchTakeMenuButtons: XInput button bits pressed since the last call).
void LrTouchSetMenuOpen(bool open);
uint32_t LrTouchTakeMenuButtons();
// True once a physical controller was used since the last call.
bool LrTouchTakePhysicalActivity();
#endif
