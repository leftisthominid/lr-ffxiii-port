// lightningreturns - guest function hooks
//
// Each hook replaces one weak recompiled function and calls the original through its
// __imp__ name.

#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <SDL3/SDL.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>
#include <fstream>
#include <set>

#include "lr_steam_saves.h"

#include <string_view>
#include <thread>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

// Same definitions as lr_guest_irq.inc puts in the generated header, so both name the same
// per-thread state.
struct LrLockSpin {
  uint64_t last_leave = 0;
  uint32_t rapid = 0;
  uint32_t guest_depth = 0;
  uint32_t host_holds = 0;
};

inline LrLockSpin& lr_lock_spin() {
  thread_local LrLockSpin spin;
  return spin;
}

REX_EXTERN(__imp__sub_829C4A60);

// sub_829C4A60 is the graphics interrupt handler the game registers with
// VdSetGraphicsInterruptCallback (0x829CFDD4). ReXGlue runs it through
// FunctionDispatcher::ExecuteInterrupt, which holds the runtime's global mutex for the
// whole call. The handler takes two kernel spinlocks (device +11032 for command-processor
// interrupts, +17120 in the vblank path sub_829CA9B8), and ReXGlue's KfAcquireSpinLock /
// KeAcquireSpinLockAtRaisedIrql spin in host code. A game thread that holds one of those
// spinlocks and then makes any kernel call or writes memory the renderer watches needs the
// global mutex: the handler spins forever holding it and the game thread waits forever for
// it, so no frame is ever presented again (movies, where the decoder threads write watched
// frame buffers all the time, hit it within seconds).
// In the default lock mode guest code no longer takes the global mutex for its "interrupts
// off" sections, so holding it around the handler protects nothing on the guest side; the
// handler runs without it, as interrupts on the console run alongside the other hardware
// threads. LR_LOCK_MODE=global keeps ReXGlue's behaviour (and counts the hold, so a guest
// spin inside the handler can hand it over).
REX_HOOK_RAW(sub_829C4A60) {
  static const bool global_mode = [] {
    const char* mode = std::getenv("LR_LOCK_MODE");
    return mode && (mode[0] == 'g' || mode[0] == 'G');
  }();
  if (global_mode) {
    auto& spin = lr_lock_spin();
    ++spin.host_holds;
    __imp__sub_829C4A60(ctx, base);
    --spin.host_holds;
    return;
  }
  auto& global = rex::thread::global_critical_region::mutex();
  global.unlock();
  __imp__sub_829C4A60(ctx, base);
  global.lock();
}

// Title menu (GuiTitleMenu, page 1). sub_827F2C10(menu, page) hides entries through the
// item's vtable+0x18 (sub_82308428: nonzero sets +0xBC bit 0x2 = hidden) and then lays the
// visible ones out in the order of the table at 0x8212D9F0. On the normal path the entries
// this port does not need are hidden by flags in [[0x82F62030]+0x5FC]:
//   item 4 INSTALL                hidden when 0x01000000 is set (game installed)
//   item 5 DOWNLOADABLE CONTENT   hidden when 0x4 is set
// (item 3 NEWS & UPDATES is hidden while sub_82925FD0() returns 0, as it does here.)
// The flags are set for the duration of the page 1 build, so the game hides the entries and
// closes up the list itself. Item 5 stays only with LR_TITLE_QUIT=1 (the old Quit to Desktop
// entry, see "Title screen: Quit to Desktop" below). LR_TITLE_MENU_FULL=1 keeps the original menu.
namespace {
constexpr uint32_t kTitleFlagsHolder = 0x82F62030;
constexpr uint32_t kTitleFlagsOffset = 0x5FC;
constexpr uint32_t kTitleHideInstall = 0x01000000;
constexpr uint32_t kTitleHideDlc = 0x4;

bool lr_title_menu_full() {
  static const bool full = [] {
    const char* v = std::getenv("LR_TITLE_MENU_FULL");
    return v && v[0] == '1';
  }();
  return full;
}

// The title menu's DOWNLOADABLE CONTENT entry (item 5) is hidden, like INSTALL. It used to become
// QUIT TO DESKTOP (relabelled after the game drew it, which flickered); LR_TITLE_QUIT=1 brings that
// back.
bool lr_title_quit_enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("LR_TITLE_QUIT");
    return v && v[0] == '1' && !lr_title_menu_full();
  }();
  return enabled;
}

uint32_t lr_load_be32(uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + address));
}

uint64_t lr_now_us() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
}  // namespace

REX_EXTERN(__imp__sub_827F2C10);
REX_HOOK_RAW(sub_827F2C10) {
  uint32_t menu = ctx.r3.u32;
  if (ctx.r4.u32 != 1 || lr_title_menu_full()) {
    __imp__sub_827F2C10(ctx, base);
    return;
  }
  uint32_t holder = lr_load_be32(base, kTitleFlagsHolder);
  uint32_t* flags_word = holder ? reinterpret_cast<uint32_t*>(base + holder + kTitleFlagsOffset) : nullptr;
  // The game updates this word from other threads, so only the two bits are touched,
  // atomically, and only the ones this hook set are cleared again.
  const uint32_t hide_be =
      __builtin_bswap32(kTitleHideInstall | (lr_title_quit_enabled() ? 0u : kTitleHideDlc));
  uint32_t before_be = 0;
  if (flags_word) before_be = std::atomic_ref<uint32_t>(*flags_word).fetch_or(hide_be);
  uint32_t added_be = flags_word ? ~before_be & hide_be : 0;
  __imp__sub_827F2C10(ctx, base);
  if (added_be) std::atomic_ref<uint32_t>(*flags_word).fetch_and(~added_be);

  void LrResolutionReachedTitle();
  LrResolutionReachedTitle();
  // Page p is the p-th node of the list at [[menu+0x44]+0x14] (linked through +0x20) and
  // its entries are the nodes at [page+0x14]. Logged once so the result can be checked.
  static std::atomic<bool> logged{false};
  if (logged.exchange(true)) return;
  uint32_t list = lr_load_be32(base, menu + 0x44);
  uint32_t page = list ? lr_load_be32(base, list + 0x14) : 0;
  if (page) page = lr_load_be32(base, page + 0x20);
  uint32_t item = page ? lr_load_be32(base, page + 0x14) : 0;
  char shown[8] = "-------";
  for (int i = 0; item && i < 7; ++i, item = lr_load_be32(base, item + 0x20)) {
    shown[i] = (lr_load_be32(base, item + 0xBC) & 0x2) ? '-' : char('0' + i);
  }
  REXLOG_INFO("LR title menu: hid INSTALL (4){}; flags were {:08X}; shown items {}",
              lr_title_quit_enabled() ? ", DOWNLOADABLE CONTENT (5) is QUIT TO DESKTOP"
                                      : " and DOWNLOADABLE CONTENT (5)",
              __builtin_bswap32(before_be), shown);
}

// ---------------------------------------------------------------------------------------
// PS3 cinematics. The 360 movies are Bink 2 files, 1280x736 with the picture in the top
// 720 rows (sub_82D1BDF0 crops 736 to 720); the PS3 release has the same 21 movies as
// 1280x720 H.264 at a much higher bit rate. Bink keeps playing the 360 file (audio, timing,
// skipping, and looping stay the game's own); each time a frame finishes decoding, the same
// frame of the PS3 movie is written over it in the game's frame buffers.
//
//   sub_82E8E658  BinkOpen(name, options, flags): name is 8 + 16 characters '@'+nibble
//                 encoding the pack file handle and the movie's byte offset in the pack
//                 (sub_82D1A630)
//   sub_82E8B078  BinkRegisterFrameBuffers(bink, BINKFRAMEBUFFERS*)
//   sub_82E89178  BinkDoFrameAsyncWait(bink, us): -1 = until the frame is done
//   sub_82E8F3C0  BinkClose(bink)
// HBINK: +0 width, +4 height, +8 frames, +12 current frame (1-based), +20/+24 frame rate.
// BINKFRAMEBUFFERS: +0 count, +4/+8 Y size, +12/+16 chroma size, +20 index of the buffer
// just decoded, +24 buffers of 48 bytes: Y, Cr, Cb, A planes of {allocate, address, pitch}.
// The game's movie shader converts full-range BT.601 (constants at 0x821E0C5C), so ffmpeg
// converts the PS3 video's limited-range BT.709 to that.
//
// LR_PS3_MOVIES = folder with <name>.mp4 (LR_PS3_Movies.ps3 makes them); unset = 360 movies.
// LR_FFMPEG = ffmpeg executable (default: ffmpeg.exe next to the game, then PATH).
// ---------------------------------------------------------------------------------------
#include <condition_variable>
#include <shared_mutex>
#include <functional>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <csignal>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <link.h>
#include <spawn.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#include <unistd.h>
extern char** environ;
#endif

namespace {

struct LrMovieEntry {
  const char* name;
  uint32_t sector;  // position in the 360 pack, 2048-byte units
  uint32_t size;    // 360 .bik size
};

// From movielistu / movielist2u / movielist3u (.x360.bin).
constexpr LrMovieEntry kLrMovies[] = {
    {"GEN_us", 0x0, 0x9c29e44},          {"OPA_us", 0x13854, 0x1df694c4},
    {"tm_lxaa_010_us", 0x4f727, 0x779470}, {"tm_lxaa_160_us", 0x5061a, 0xe636e0},
    {"tm_lxaa_090_us", 0x522e1, 0x984898}, {"hm_lxaa_150_us", 0x535eb, 0x268ce0},
    {"hm_lxaa_190_us", 0x53abd, 0x75b35c}, {"ENB_us", 0x0, 0x199f95ac},
    {"ENS_us", 0x333f3, 0xae2a580},       {"hm_wlaa_175_us", 0x0, 0x10d7e4},
    {"hm_edaa_191_us", 0x21b, 0x178950},   {"hm_edaa_190_us", 0x50d, 0x24ffd4},
    {"ENA_us", 0x9ad, 0x25b8e54c},         {"hm_ptaa_200_us", 0x4c0ca, 0xded0c},
    {"hm_ptaa_005_us", 0x4c288, 0x592228}, {"hm_ptaa_006_us", 0x4cdad, 0x5488d0},
    {"hm_ptaa_150_us", 0x4d83f, 0x406efc}, {"hm_ptaa_020_us", 0x4e04d, 0x58394c},
    {"hm_ptaa_130_us", 0x4eb55, 0x9da044}, {"hm_ptaa_120_us", 0x4ff0a, 0x76b0dc},
    {"hm_edaa_020_us", 0x50de1, 0xd37a0},
};

std::filesystem::path lr_env_path(const char* name) {
#if defined(_WIN32)
  std::wstring wname(name, name + std::strlen(name));
  if (const wchar_t* v = _wgetenv(wname.c_str()); v && *v) return std::filesystem::path(v);
#else
  if (const char* v = std::getenv(name); v && *v) return std::filesystem::path(v);
#endif
  return {};
}

const std::filesystem::path& lr_movie_dir() {
  static const std::filesystem::path dir = lr_env_path("LR_PS3_MOVIES");
  return dir;
}

std::filesystem::path lr_ffmpeg_path() {
  std::filesystem::path p = lr_env_path("LR_FFMPEG");
  if (!p.empty()) return p;
#if defined(_WIN32)
  wchar_t exe[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (n && n < MAX_PATH) {
    std::filesystem::path local = std::filesystem::path(exe).parent_path() / L"ffmpeg.exe";
    std::error_code ec;
    if (std::filesystem::exists(local, ec)) return local;
  }
  return L"ffmpeg.exe";
#else
  return "ffmpeg";
#endif
}

// An ffmpeg child process writing raw yuv420p frames to a pipe.
class LrFfmpegPipe {
 public:
  ~LrFfmpegPipe() { Stop(); }

  bool Start(const std::filesystem::path& ffmpeg, const std::filesystem::path& file, double seek_seconds,
             uint32_t width, uint32_t height, double fps) {
    // One output frame per frame of the 360 movie: the game asks for frames by the Bink frame
    // number, and a PS3 stream at another rate would drift and run out before the movie ends.
    char vf[320];
    std::snprintf(vf, sizeof(vf),
                  "fps=fps=%.6f:round=near,"
                  "scale=w=%u:h=%u:in_color_matrix=bt709:out_color_matrix=bt601:in_range=tv:out_range=pc:"
                  "flags=bicubic,format=yuv420p",
                  fps, width, height);
    char ss[32];
    std::snprintf(ss, sizeof(ss), "%.4f", seek_seconds);
    std::vector<std::string> args = {"-hide_banner", "-loglevel", "error", "-nostdin", "-threads", "2"};
    if (seek_seconds > 0) {
      args.push_back("-ss");
      args.push_back(ss);
    }
    std::vector<std::string> tail = {"-map", "0:v:0", "-an", "-sn", "-vf", vf, "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"};
#if defined(_WIN32)
    auto quote = [](const std::wstring& a) { return L"\"" + a + L"\""; };
    auto widen = [](const std::string& a) { return std::wstring(a.begin(), a.end()); };
    std::wstring cmd = quote(ffmpeg.wstring());
    for (auto& a : args) cmd += L" " + widen(a);
    cmd += L" -i " + quote(file.wstring());
    for (auto& a : tail) cmd += L" " + (a == vf ? quote(widen(a)) : widen(a));

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 4 << 20)) return false;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    std::filesystem::path log = std::filesystem::temp_directory_path() / L"lr_movie_ffmpeg.log";
    HANDLE err = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = write_end;
    si.hStdError = err != INVALID_HANDLE_VALUE ? err : nullptr;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(write_end);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!ok) {
      CloseHandle(read_end);
      return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    read_ = read_end;
    return true;
#else
    int fds[2];
    if (pipe(fds) != 0) return false;
    std::vector<std::string> all = {ffmpeg.string()};
    all.insert(all.end(), args.begin(), args.end());
    all.push_back("-i");
    all.push_back(file.string());
    all.insert(all.end(), tail.begin(), tail.end());
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/tmp/lr_movie_ffmpeg.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
      close(fds[0]);
      return false;
    }
    pid_ = pid;
    fd_ = fds[0];
    return true;
#endif
  }

  // Reads exactly size bytes; false at the end of the stream or on an error.
  bool Read(uint8_t* dst, size_t size) {
    size_t done = 0;
    while (done < size) {
#if defined(_WIN32)
      DWORD got = 0;
      DWORD want = DWORD((std::min<size_t>)(size - done, 1u << 20));
      if (!read_ || !ReadFile(read_, dst + done, want, &got, nullptr) || got == 0) return false;
#else
      if (fd_ < 0) return false;
      ssize_t got = read(fd_, dst + done, size - done);
      if (got <= 0) return false;
#endif
      done += size_t(got);
    }
    return true;
  }

  void Stop() {
#if defined(_WIN32)
    if (process_) {
      TerminateProcess(process_, 0);
      WaitForSingleObject(process_, 2000);
      CloseHandle(process_);
      process_ = nullptr;
    }
    if (read_) {
      CloseHandle(read_);
      read_ = nullptr;
    }
#else
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      waitpid(pid_, nullptr, 0);
      pid_ = -1;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
#endif
  }

  // Unblocks a Read on another thread (the reader then calls Stop).
  void Interrupt() {
#if defined(_WIN32)
    if (process_) TerminateProcess(process_, 0);
#else
    if (pid_ > 0) kill(pid_, SIGKILL);
#endif
  }

 private:
#if defined(_WIN32)
  HANDLE process_ = nullptr;
  HANDLE read_ = nullptr;
#else
  pid_t pid_ = -1;
  int fd_ = -1;
#endif
};

// One ffmpeg run from a start frame, decoding ahead on its own thread. That thread also
// starts and ends the ffmpeg process, so a game thread never waits for a process to start
// or exit; it only takes frames that are ready.
class LrMovieDecoder {
 public:
  LrMovieDecoder(const std::filesystem::path& file, uint32_t first, uint32_t width, uint32_t height, double fps)
      : frame_bytes_(size_t(width) * height * 3 / 2), next_number_(first) {
    worker_ = std::thread([this, file, first, width, height, fps] { Run(file, first, width, height, fps); });
  }
  ~LrMovieDecoder() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
      if (pipe_) pipe_->Interrupt();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  uint32_t next_number() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_number_;
  }

  // The PS3 video has no more frames (or ffmpeg could not run).
  bool ended() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ended_ && ready_.empty();
  }

  // Moves frame n into out; false if it is not decoded within `wait`, or past the end of the
  // PS3 video (then the 360 frames play on rather than the last PS3 frame freezing).
  bool Take(uint32_t n, std::vector<uint8_t>& out, uint32_t& out_number, std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = std::chrono::steady_clock::now() + wait;
    bool got = false;
    while (next_number_ <= n) {
      if (!cv_.wait_until(lock, deadline, [&] { return !ready_.empty() || ended_; })) return got && out_number == n;
      if (ready_.empty()) return got && out_number == n;  // ended
      out.swap(ready_.front());
      spare_.push_back(std::move(ready_.front()));
      ready_.pop_front();
      out_number = next_number_++;
      got = true;
      cv_.notify_all();
    }
    return got;
  }

 private:
  void Run(std::filesystem::path file, uint32_t first, uint32_t width, uint32_t height, double fps) {
    auto pipe = std::make_unique<LrFfmpegPipe>();
    // Seek a little before the frame so rounding cannot skip it; ffmpeg decodes from the
    // previous key frame and drops everything before the seek time.
    double seek = first > 1 ? (double(first) - 1.5) / fps : 0.0;
    std::filesystem::path ffmpeg = lr_ffmpeg_path();
    if (!pipe->Start(ffmpeg, file, seek, width, height, fps)) {
      REXLOG_WARN("LR movie: could not start {}", ffmpeg.string());
      std::lock_guard<std::mutex> lock(mutex_);
      ended_ = true;
      cv_.notify_all();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pipe_ = pipe.get();
      if (stop_) pipe_->Interrupt();
    }
    for (;;) {
      std::vector<uint8_t> frame;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return stop_ || ready_.size() < kAhead; });
        if (stop_) break;
        if (!spare_.empty()) {
          frame = std::move(spare_.back());
          spare_.pop_back();
        }
      }
      frame.resize(frame_bytes_);
      bool ok = pipe->Read(frame.data(), frame.size());
      std::lock_guard<std::mutex> lock(mutex_);
      if (!ok || stop_) {
        ended_ = true;
        cv_.notify_all();
        break;
      }
      ready_.push_back(std::move(frame));
      cv_.notify_all();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pipe_ = nullptr;
    }
    pipe->Stop();
  }

  static constexpr size_t kAhead = 6;
  size_t frame_bytes_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::vector<uint8_t>> ready_;
  std::vector<std::vector<uint8_t>> spare_;
  uint32_t next_number_;  // number of ready_.front()
  bool ended_ = false, stop_ = false;
  LrFfmpegPipe* pipe_ = nullptr;
  std::thread worker_;
};

// Destroys on a background thread: stopping ffmpeg and joining the decoder thread never
// holds up the game thread that closes or seeks a movie.
template <typename T>
void lr_retire(std::unique_ptr<T> object) {
  if (object) std::thread([o = std::move(object)]() mutable { o.reset(); }).detach();
}

// What the PS3 movie path was doing last; logged when frames stop ("LR hang").
struct LrMovieTrace {
  std::atomic<const char*> name{nullptr};
  std::atomic<uint32_t> frame{0}, shown{0}, late{0}, restarts{0};
  std::atomic<uint64_t> last_call_us{0}, last_write_us{0};
  std::atomic<bool> in_get{false}, in_write{false}, open{false};
} g_lr_movie_trace;

// The PS3 video of one movie. Get(n) returns frame n (1-based) or nullptr if it is not
// ready in time, in which case the 360 frame stays on screen. A jump (seek, loop) starts a
// new decoder at that frame.
class LrPs3Movie {
 public:
  LrPs3Movie(std::string name, std::filesystem::path file, uint32_t width, uint32_t height, double fps)
      : name_(std::move(name)), file_(std::move(file)), width_(width), height_(height), fps_(fps) {}
  ~LrPs3Movie() { lr_retire(std::move(decoder_)); }

  const std::string& name() const { return name_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t late() const { return late_; }
  uint32_t after_end() const { return after_end_; }

  void Start() { Restart(1); }

  // Never blocks: the caller is a game thread holding game locks. A frame that is not
  // decoded yet leaves the 360 frame on screen for that one frame.
  const uint8_t* Get(uint32_t n) {
    if (n == 0) n = 1;
    if (n == current_number_ && !current_.empty()) return current_.data();
    uint32_t next = decoder_ ? decoder_->next_number() : 0;
    auto now = std::chrono::steady_clock::now();
    // Past the end of the PS3 video: nothing to restart (the 360 frames play on).
    bool past_end = decoder_ && n >= next && decoder_->ended();
    bool jump = !past_end && (!decoder_ || n < next || n > next + uint32_t(fps_ * 4));
    // At most one restart per second: a restart that has not produced frames yet must not
    // be replaced by the next one over and over.
    if (jump && (!decoder_ || now - last_restart_ > std::chrono::seconds(1))) {
      Restart(n);
      last_restart_ = now;
      g_lr_movie_trace.restarts.fetch_add(1, std::memory_order_relaxed);
    }
    bool ok = decoder_->Take(n, current_, current_number_, std::chrono::milliseconds(0));
    if (!ok) {
      if (decoder_->ended()) {
        ++after_end_;  // past the end of the PS3 video: the 360 frames play on
        return nullptr;
      }
      ++late_;
      // A frame that is merely late repeats the last PS3 frame (a few frames at most) rather
      // than flashing the 360 one, whose colours differ.
      if (!current_.empty() && ++held_ <= 5) return current_.data();
      return nullptr;
    }
    held_ = 0;
    fresh_ = false;
    return current_.empty() ? nullptr : current_.data();
  }

 private:
  void Restart(uint32_t first) {
    lr_retire(std::move(decoder_));
    decoder_ = std::make_unique<LrMovieDecoder>(file_, first, width_, height_, fps_);
    current_.clear();
    current_number_ = 0;
    fresh_ = true;
  }

  std::string name_;
  std::filesystem::path file_;
  uint32_t width_, height_;
  double fps_;
  std::unique_ptr<LrMovieDecoder> decoder_;
  std::vector<uint8_t> current_;
  uint32_t current_number_ = 0;
  bool fresh_ = true;
  uint32_t late_ = 0;
  uint32_t after_end_ = 0;  // frames the 360 movie had after the PS3 video ended
  uint32_t held_ = 0;  // late frames in a row shown as the previous PS3 frame
  std::chrono::steady_clock::time_point last_restart_{};
};

struct LrMovieSession {
  std::unique_ptr<LrPs3Movie> movie;
  uint32_t frame_buffers = 0;  // guest BINKFRAMEBUFFERS*
  uint32_t last_frame = 0, last_index = ~0u;
  std::atomic<uint32_t> written{0};
  uint32_t logged = 0;
  std::atomic<bool> closed{false};   // BinkClose started: the buffers are about to be freed
  std::atomic<bool> writing{false};  // the writer thread is copying into this movie's buffers
};

// Copies PS3 frames into the game's frame buffers on its own thread. The game thread that
// calls BinkDoFrameAsyncWait holds game locks; a write into memory the GPU watches makes
// the runtime take its global lock, which the graphics interrupt holds while it waits for
// one of those game locks - a deadlock (the New Game freeze). This thread holds no game
// locks, and the game thread never waits for it (BinkClose waits at most 50 ms).
class LrMovieWriter {
 public:
  struct Plane {
    uint32_t address, pitch, cols, rows, src_offset, src_pitch;
  };
  struct Job {
    std::shared_ptr<LrMovieSession> session;
    uint8_t* base;
    std::vector<uint8_t> frame;
    Plane planes[3];
  };

  static LrMovieWriter& Get() {
    static LrMovieWriter* writer = new LrMovieWriter();  // never destroyed: lives until exit
    return *writer;
  }

  // Takes a spare frame buffer to fill (avoids allocating 1.4 MB per frame).
  std::vector<uint8_t> Spare() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (spare_.empty()) return {};
    auto v = std::move(spare_.back());
    spare_.pop_back();
    return v;
  }

  void Submit(Job job) {
    std::lock_guard<std::mutex> lock(mutex_);
    // A frame that is still waiting is replaced: only the newest matters.
    while (queue_.size() >= 2) {
      spare_.push_back(std::move(queue_.front().frame));
      queue_.pop_front();
    }
    queue_.push_back(std::move(job));
    cv_.notify_one();
  }

 private:
  LrMovieWriter() {
    std::thread([this] { Run(); }).detach();
  }

  void Run() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return !queue_.empty(); });
        job = std::move(queue_.front());
        queue_.pop_front();
      }
      LrMovieSession& s = *job.session;
      s.writing.store(true);
      g_lr_movie_trace.in_write.store(true, std::memory_order_relaxed);
      if (!s.closed.load()) {
        bool complete = true;
        for (const Plane& p : job.planes) {
          for (uint32_t y = 0; y < p.rows; ++y) {
            if (s.closed.load(std::memory_order_relaxed)) {
              complete = false;
              break;
            }
            std::memcpy(job.base + p.address + y * p.pitch, job.frame.data() + p.src_offset + size_t(y) * p.src_pitch,
                        p.cols);
          }
          if (!complete) break;
        }
        if (complete) {
          s.written.fetch_add(1);
          g_lr_movie_trace.shown.fetch_add(1, std::memory_order_relaxed);
          g_lr_movie_trace.last_write_us.store(lr_now_us(), std::memory_order_relaxed);
        }
      }
      g_lr_movie_trace.in_write.store(false, std::memory_order_relaxed);
      s.writing.store(false);
      std::lock_guard<std::mutex> lock(mutex_);
      if (spare_.size() < 4) spare_.push_back(std::move(job.frame));
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  std::vector<std::vector<uint8_t>> spare_;
};

std::mutex g_lr_movie_mutex;
std::unordered_map<uint32_t, std::shared_ptr<LrMovieSession>> g_lr_movies;  // by HBINK

std::shared_ptr<LrMovieSession> lr_movie_session(uint32_t bink) {
  std::lock_guard<std::mutex> lock(g_lr_movie_mutex);
  auto it = g_lr_movies.find(bink);
  return it == g_lr_movies.end() ? nullptr : it->second;
}

// Same crop as sub_82D1BDF0.
uint32_t lr_movie_visible_height(uint32_t h) {
  switch (h) {
    case 768: case 736: return 720;
    case 384: case 368: return 360;
    case 192: case 184: return 180;
    default: return h;
  }
}

}  // namespace

REX_EXTERN(__imp__sub_82E8E658);
REX_HOOK_RAW(sub_82E8E658) {
  const uint32_t name_address = ctx.r3.u32;
  uint32_t handle = 0;
  uint64_t offset = 0;
  bool encoded = true;
  for (int i = 0; i < 24; ++i) {
    uint8_t c = base[name_address + i];
    if (c < 0x40 || c > 0x4F) {
      encoded = false;
      break;
    }
    if (i < 8) handle |= uint32_t(c - 0x40) << (4 * i);
    else offset |= uint64_t(c - 0x40) << (4 * (i - 8));
  }
  REXLOG_INFO("LR movie: BinkOpen (pack offset {:X})", offset);
  __imp__sub_82E8E658(ctx, base);
  const uint32_t bink = ctx.r3.u32;
  REXLOG_INFO("LR movie: BinkOpen returned {:08X}", bink);
  if (!bink || !encoded || lr_movie_dir().empty()) return;

  const uint32_t width = lr_load_be32(base, bink + 0), height = lr_load_be32(base, bink + 4);
  const uint32_t frames = lr_load_be32(base, bink + 8);
  const uint32_t rate = lr_load_be32(base, bink + 20), rate_div = lr_load_be32(base, bink + 24);
  // Which movie: by its offset in the pack; the three at offset 0 (one per pack) by the
  // file size, which HBINK holds in one of its first words.
  std::vector<const LrMovieEntry*> candidates;
  for (const auto& m : kLrMovies) {
    if (uint64_t(m.sector) * 2048 == offset) candidates.push_back(&m);
  }
  const LrMovieEntry* match = candidates.size() == 1 ? candidates[0] : nullptr;
  if (!match) {
    if (candidates.empty()) {
      for (const auto& m : kLrMovies) candidates.push_back(&m);
    }
    for (const LrMovieEntry* m : candidates) {
      for (uint32_t w = 0; w < 32 && !match; ++w) {
        uint32_t v = lr_load_be32(base, bink + w * 4);
        if (v + 16 >= m->size && v <= m->size + 16) match = m;
      }
      if (match) break;
    }
  }
  if (!match) {
    REXLOG_INFO("LR movie: unknown movie (pack offset {:X}, {}x{}, {} frames); playing the 360 video", offset,
                width, height, frames);
    return;
  }
  std::filesystem::path file = lr_movie_dir() / (std::string(match->name) + ".mp4");
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    REXLOG_INFO("LR movie: {} - no PS3 file {}; playing the 360 video", match->name, file.string());
    return;
  }
  double fps = (rate && rate_div) ? double(rate) / double(rate_div) : 30000.0 / 1001.0;
  uint32_t visible = lr_movie_visible_height(height);
  auto session = std::make_shared<LrMovieSession>();
  session->movie = std::make_unique<LrPs3Movie>(match->name, file, width, visible, fps);
  session->movie->Start();
  REXLOG_INFO("LR movie: {} ({}x{}, {} frames, {:.3f} fps) -> PS3 video {}", match->name, width, height, frames, fps,
              file.string());
  g_lr_movie_trace.name.store(match->name, std::memory_order_relaxed);
  g_lr_movie_trace.open.store(true, std::memory_order_relaxed);
  g_lr_movie_trace.frame.store(0);
  g_lr_movie_trace.shown.store(0);
  g_lr_movie_trace.late.store(0);
  g_lr_movie_trace.restarts.store(0);
  std::lock_guard<std::mutex> lock(g_lr_movie_mutex);
  g_lr_movies[bink] = std::move(session);
}

REX_EXTERN(__imp__sub_82E8B078);
REX_HOOK_RAW(sub_82E8B078) {
  const uint32_t bink = ctx.r3.u32, frame_buffers = ctx.r4.u32;
  __imp__sub_82E8B078(ctx, base);
  if (auto s = lr_movie_session(bink)) s->frame_buffers = frame_buffers;
}

REX_EXTERN(__imp__sub_82E89178);
REX_HOOK_RAW(sub_82E89178) {
  const uint32_t bink = ctx.r3.u32;
  const int32_t wait_us = int32_t(ctx.r4.u32);
  __imp__sub_82E89178(ctx, base);
  if (wait_us != -1 && !ctx.r3.u32) return;  // still decoding
  auto s = lr_movie_session(bink);
  if (!s || !s->frame_buffers) return;
  const uint32_t fb = s->frame_buffers;
  const uint32_t count = lr_load_be32(base, fb + 0);
  const uint32_t index = lr_load_be32(base, fb + 20);
  const uint32_t frame = lr_load_be32(base, bink + 12);
  if (index >= count || index >= 8) return;
  if (frame == s->last_frame && index == s->last_index) return;
  g_lr_movie_trace.frame.store(frame, std::memory_order_relaxed);
  g_lr_movie_trace.last_call_us.store(lr_now_us(), std::memory_order_relaxed);
  g_lr_movie_trace.in_get.store(true, std::memory_order_relaxed);
  const uint8_t* yuv = s->movie->Get(frame);
  g_lr_movie_trace.in_get.store(false, std::memory_order_relaxed);
  if (!yuv) {
    g_lr_movie_trace.late.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  s->last_frame = frame;
  s->last_index = index;

  const uint32_t w = s->movie->width(), h = s->movie->height();
  const uint32_t set = fb + 24 + index * 48;
  const uint32_t yw = lr_load_be32(base, fb + 4), yh = lr_load_be32(base, fb + 8);
  const uint32_t cw = lr_load_be32(base, fb + 12), ch = lr_load_be32(base, fb + 16);
  auto plane = [&](uint32_t plane_offset, uint32_t src_offset, uint32_t src_w, uint32_t src_h, uint32_t buf_w,
                   uint32_t buf_h) {
    LrMovieWriter::Plane p{};
    p.address = lr_load_be32(base, set + plane_offset + 4);
    p.pitch = lr_load_be32(base, set + plane_offset + 8);
    if (p.address && p.pitch) {
      p.cols = (std::min)({src_w, buf_w, p.pitch});
      p.rows = (std::min)(src_h, buf_h);
    }
    p.src_offset = src_offset;
    p.src_pitch = src_w;
    return p;
  };
  const size_t frame_bytes = size_t(w) * h * 3 / 2;
  LrMovieWriter& writer = LrMovieWriter::Get();
  LrMovieWriter::Job job;
  job.session = s;
  job.base = base;
  job.frame = writer.Spare();
  job.frame.resize(frame_bytes);
  std::memcpy(job.frame.data(), yuv, frame_bytes);  // host memory: no GPU watch involved
  const uint32_t u_offset = w * h, v_offset = u_offset + (w / 2) * (h / 2);
  job.planes[0] = plane(0, 0, w, h, yw, yh);                     // Y
  job.planes[1] = plane(12, v_offset, w / 2, h / 2, cw, ch);     // Cr
  job.planes[2] = plane(24, u_offset, w / 2, h / 2, cw, ch);     // Cb
  writer.Submit(std::move(job));
  if (s->logged < 3) {
    ++s->logged;
    REXLOG_INFO("LR movie: {} frame {} -> buffer {} (Y {}x{} pitch {}, chroma {}x{})", s->movie->name(), frame, index,
                yw, yh, lr_load_be32(base, set + 8), cw, ch);
  }
}

REX_EXTERN(__imp__sub_82E8F3C0);
REX_HOOK_RAW(sub_82E8F3C0) {
  const uint32_t bink = ctx.r3.u32;
  std::shared_ptr<LrMovieSession> s;
  {
    std::lock_guard<std::mutex> lock(g_lr_movie_mutex);
    auto it = g_lr_movies.find(bink);
    if (it != g_lr_movies.end()) {
      s = std::move(it->second);
      g_lr_movies.erase(it);
    }
  }
  if (s) {
    s->closed.store(true);
    // The writer checks `closed` before every row; give an in-flight copy a moment to stop.
    // Bounded, so this can never hang the game.
    for (int i = 0; i < 200 && s->writing.load(); ++i) std::this_thread::sleep_for(std::chrono::microseconds(250));
    REXLOG_INFO("LR movie: {} closed; {} PS3 frames shown, {} late, {} after the PS3 video ended",
                s->movie->name(), s->written.load(), s->movie->late(), s->movie->after_end());
    // The decoder is stopped on a background thread (LrPs3Movie's destructor retires it).
    s->movie.reset();
  }
  __imp__sub_82E8F3C0(ctx, base);
  g_lr_movie_trace.open.store(false, std::memory_order_relaxed);
  REXLOG_INFO("LR movie: BinkClose done");
}

// ---------------------------------------------------------------------------------------
// Quit to title. The game returns to its title screen the way it did on the 360: the main
// loop (sub_82ABEE80) ends and sub_82ABEB30 calls XLaunchNewImage("default.xex")
// (sub_82D3DA70 -> XamLoaderLaunchTitle) to start the executable again. ReXGlue's
// XamLoaderLaunchTitle only terminates the title, so the game sat on a black screen.
// Here a relaunch of an .xex starts this program again with the same arguments (log file
// renamed to <log>_relaunch<N>.log) and ends the current process; the new process waits
// for the old one to be gone before it starts (LR_WAIT_PID).
// ---------------------------------------------------------------------------------------
#if defined(_WIN32)
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace {

struct LrRelaunchWait {
  LrRelaunchWait() {
    const char* v = std::getenv("LR_WAIT_PID");
    if (!v || !*v) return;
    unsigned long pid = std::strtoul(v, nullptr, 10);
#if defined(_WIN32)
    SetEnvironmentVariableW(L"LR_WAIT_PID", nullptr);
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid))) {
      WaitForSingleObject(h, 15000);
      CloseHandle(h);
    }
#else
    unsetenv("LR_WAIT_PID");
    for (int i = 0; i < 1500 && pid && kill(pid_t(pid), 0) == 0; ++i) usleep(10000);
#endif
  }
};
LrRelaunchWait g_lr_relaunch_wait;

int lr_relaunch_generation() {
  const char* v = std::getenv("LR_RELAUNCH");
  return v ? std::atoi(v) : 0;
}

// <dir>/<stem>_relaunch<N><ext>, from the original log name.
std::filesystem::path lr_relaunch_log_name(const std::filesystem::path& log, int generation) {
  std::string stem = log.stem().string();
  if (auto pos = stem.find("_relaunch"); pos != std::string::npos) stem.resize(pos);
  return log.parent_path() / (stem + "_relaunch" + std::to_string(generation) + log.extension().string());
}

bool lr_restart_process() {
  const int generation = lr_relaunch_generation() + 1;
#if defined(_WIN32)
  wchar_t exe[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (!n || n >= MAX_PATH) return false;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return false;
  // Quoting that CommandLineToArgvW / the CRT read back as the same argument.
  auto quote = [](const std::wstring& a) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
      if (c == L'\\') {
        ++slashes;
        continue;
      }
      out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
      slashes = 0;
      out.push_back(c);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
  };
  std::wstring cmd = quote(exe);
  const std::wstring log_flag = L"--log_file=";
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a.rfind(log_flag, 0) == 0) a = log_flag + lr_relaunch_log_name(a.substr(log_flag.size()), generation).wstring();
    cmd += L" " + quote(a);
  }
  LocalFree(argv);
  SetEnvironmentVariableW(L"LR_WAIT_PID", std::to_wstring(GetCurrentProcessId()).c_str());
  SetEnvironmentVariableW(L"LR_RELAUNCH", std::to_wstring(generation).c_str());
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
  SetEnvironmentVariableW(L"LR_WAIT_PID", nullptr);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#elif defined(__ANDROID__)
  return LrAndroidRestartProcess(generation);  // android/game/lr_android_main.cpp
#else
  std::vector<std::string> args;
  if (FILE* f = std::fopen("/proc/self/cmdline", "rb")) {
    std::string cur;
    for (int c; (c = std::fgetc(f)) != EOF;) {
      if (c == 0) {
        args.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(char(c));
      }
    }
    std::fclose(f);
  }
  if (args.empty()) return false;
  const std::string log_flag = "--log_file=";
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i].rfind(log_flag, 0) == 0)
      args[i] = log_flag + lr_relaunch_log_name(args[i].substr(log_flag.size()), generation).string();
  }
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  setenv("LR_WAIT_PID", std::to_string(getpid()).c_str(), 1);
  setenv("LR_RELAUNCH", std::to_string(generation).c_str(), 1);
  pid_t pid = 0;
  int rc = posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, argv.data(), environ);
  unsetenv("LR_WAIT_PID");
  return rc == 0;
#endif
}

[[noreturn]] void lr_end_process() {
  std::fflush(nullptr);
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), 0);
#endif
  std::_Exit(0);
}

}  // namespace

// The main loop (sub_82ABEE80) runs sub_82AB3AA0(app) each frame and stops when app+38 is
// set; app+39 set as well means "restart" (quit to title): it then calls the app's shutdown
// (vtable +12) and XLaunchNewImage. That shutdown never finishes here (tested: quitting to
// the title froze until a watchdog gave up after 5 s), and it only tears down what the
// process exit discards anyway - so the new process is started as soon as the game asks to
// leave. Progress is never saved by quitting, and settings are written when changed.
std::atomic<uint64_t> g_lr_quit_started_ms{0};
std::atomic<bool> g_lr_relaunching{false};

// Frame overlay (F2, lightningreturns_app.h): the start time of each of the game's frames,
// written by the main loop below (one writer), read by the UI thread.
namespace {
constexpr uint32_t kLrFrameRing = 512;
std::array<std::atomic<uint64_t>, kLrFrameRing> g_lr_frame_start_ns{};
std::atomic<uint32_t> g_lr_frame_starts{0};
}  // namespace

int LrRecentFrameTimesMs(float* out, int max) {
  uint32_t n = g_lr_frame_starts.load(std::memory_order_acquire);
  uint32_t frames = std::min<uint32_t>({n ? n - 1 : 0, uint32_t(max), kLrFrameRing - 1});
  for (uint32_t k = 0; k < frames; ++k) {
    uint32_t i = n - frames + k;  // frame i ends where frame i + 1 starts
    uint64_t a = g_lr_frame_start_ns[(i - 1) % kLrFrameRing].load(std::memory_order_relaxed);
    uint64_t b = g_lr_frame_start_ns[i % kLrFrameRing].load(std::memory_order_relaxed);
    out[k] = b > a ? float(double(b - a) / 1e6) : 0.0f;
  }
  return int(frames);
}

REX_EXTERN(__imp__sub_82AB3AA0);
REX_HOOK_RAW(sub_82AB3AA0) {
  uint32_t app = ctx.r3.u32;
  {
    uint32_t i = g_lr_frame_starts.load(std::memory_order_relaxed);
    g_lr_frame_start_ns[i % kLrFrameRing].store(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now().time_since_epoch()).count()),
        std::memory_order_relaxed);
    g_lr_frame_starts.store(i + 1, std::memory_order_release);
  }
  __imp__sub_82AB3AA0(ctx, base);
  if (!app || !base[app + 38] || !base[app + 39] || g_lr_quit_started_ms.load() ||
      std::getenv("LR_NO_RELAUNCH")) {
    return;
  }
  uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
  g_lr_quit_started_ms.store(now);
  if (g_lr_relaunching.exchange(true)) {
    return;
  }
  REXLOG_INFO("LR relaunch: the game is leaving to the title screen; starting a new process now "
              "(the game's own shutdown is skipped)");
  ::rex::FlushLogging();
  if (lr_restart_process()) lr_end_process();
  REXLOG_WARN("LR relaunch: could not start a new process; letting the game shut down");
  g_lr_relaunching.store(false);
}

// ---------------------------------------------------------------------------------------
// Title screen: Quit to Desktop. The title menu's DOWNLOADABLE CONTENT entry (item 5, of no
// use here) is shown as QUIT TO DESKTOP and ends the game when picked:
//  - label: wherever the text "DOWNLOADABLE CONTENT" is looked up (the text table through
//    sub_826FE158, the heap copies LocalizedText::Set gets through sub_8270CA90) it reads
//    QUIT TO DESKTOP instead (only the DLC screens use that text, and they are not offered);
//  - picking it: the title script then opens the DLC screen (Window.showDLCMenu,
//    sub_82997FD0, whose screen setup is sub_827617F8); while the game is still at the title
//    either of them quits instead.
// "At the title" = this process has not started the field (White.startField_l, sub_82995268)
// nor opened the in-game main menu (sub_827A1F50) or its Settings (sub_827A13B8); quitting to
// the title starts a new process, so it is true there again. Window.checkTitleMenu
// (sub_8299BD60) results are logged (the entry picked + 1, -1 = attract timeout).
// ---------------------------------------------------------------------------------------
std::atomic<bool> g_lr_left_title{false};
bool lr_at_title() { return !g_lr_left_title.load(); }
// When the title screen last ran checkTitleMenu (steady clock, ms). With Settings open from the
// title, the title script keeps running while the menu code (sub_827A1F50) runs too: the menu
// only means "left the title" once the title screen has stopped.
std::atomic<int64_t> g_lr_title_check_ms{0};

namespace {
int64_t lr_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void lr_leave_title(const char* how, bool menu = false) {
  if (menu && lr_now_ms() - g_lr_title_check_ms.load(std::memory_order_relaxed) < 1000) return;
  if (!g_lr_left_title.exchange(true)) REXLOG_INFO("LR title: left the title screen ({})", how);
}

[[noreturn]] void lr_quit_to_desktop(const char* how) {
  REXLOG_INFO("LR title menu: Quit to Desktop ({})", how);
  ::rex::FlushLogging();
  lr_end_process();
}
}  // namespace

// For a text the game just looked up (a table entry or a heap copy, double-NUL ended).
void lr_title_quit_label(uint8_t* base, uint32_t text) {
  static constexpr char kFrom[] = "DOWNLOADABLE CONTENT";
  static constexpr char kTo[] = "QUIT TO DESKTOP";
  if (!text || !lr_title_quit_enabled()) return;
  char* s = reinterpret_cast<char*>(base + text);
  if (std::strncmp(s, kFrom, sizeof(kFrom)) != 0) return;
  std::memset(s, 0, sizeof(kFrom));
  std::memcpy(s, kTo, sizeof(kTo) - 1);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("LR title menu: DOWNLOADABLE CONTENT reads QUIT TO DESKTOP");
}

REX_EXTERN(__imp__sub_82997FD0);
REX_HOOK_RAW(sub_82997FD0) {
  if (lr_title_quit_enabled() && lr_at_title()) lr_quit_to_desktop("picked in the title menu");
  __imp__sub_82997FD0(ctx, base);
}

REX_EXTERN(__imp__sub_827617F8);
REX_HOOK_RAW(sub_827617F8) {
  if (lr_title_quit_enabled() && lr_at_title()) lr_quit_to_desktop("the title menu opened the DLC screen");
  __imp__sub_827617F8(ctx, base);
}

REX_EXTERN(__imp__sub_8299BD60);
REX_HOOK_RAW(sub_8299BD60) {
  uint32_t vm = ctx.r3.u32;
  // Only the title screen runs this. The game can come back to the title without a new process
  // (a game over, its own return to title): QUIT TO DESKTOP must work there again too.
  g_lr_title_check_ms.store(lr_now_ms(), std::memory_order_relaxed);
  if (g_lr_left_title.exchange(false)) REXLOG_INFO("LR title: back at the title screen");
  __imp__sub_8299BD60(ctx, base);
  int32_t picked = vm ? int32_t(lr_load_be32(base, lr_load_be32(base, vm + 0x958) + 4)) : 0;
  static std::atomic<int> logged{0};
  if (picked && logged.fetch_add(1) < 16) REXLOG_INFO("LR title menu: checkTitleMenu returned {}", picked);
}

REX_EXTERN(__imp__sub_82995268);
REX_HOOK_RAW(sub_82995268) {
  lr_leave_title("field started");
  __imp__sub_82995268(ctx, base);
}

REX_EXTERN(__imp__sub_827A1F50);
REX_HOOK_RAW(sub_827A1F50) {
  lr_leave_title("main menu opened", true);
  __imp__sub_827A1F50(ctx, base);
}

REX_EXTERN(__imp__sub_82D3DA70);
REX_HOOK_RAW(sub_82D3DA70) {
  std::string path;
  if (ctx.r3.u32) {
    for (uint32_t i = 0; i < 260 && base[ctx.r3.u32 + i]; ++i) path.push_back(char(base[ctx.r3.u32 + i]));
  }
  std::string lower = path;
  for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
  const bool xex = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".xex") == 0;
  if (xex && std::getenv("LR_NO_RELAUNCH") == nullptr) {
    if (g_lr_relaunching.exchange(true)) {
      return;  // the watchdog is already restarting
    }
    uint64_t started = g_lr_quit_started_ms.load();
    uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
    REXLOG_INFO("LR relaunch: the game restarts itself ({}) to return to the title screen{}; starting a new process",
                path, started ? fmt::format(" ({} ms after it started leaving)", now - started) : std::string());
    ::rex::FlushLogging();
    if (lr_restart_process()) lr_end_process();
    REXLOG_WARN("LR relaunch: could not start a new process");
  }
  __imp__sub_82D3DA70(ctx, base);
}

// ---------------------------------------------------------------------------------------
// Frame pacing. The emulated 360 vblank (60 Hz, which the game counts to hold 30 fps) is
// produced by a thread that sleeps 1 ms at a time, and the GPU thread sleeps in whole
// milliseconds while it waits for the game. Windows rounds a 1 ms sleep up to its timer
// period, 15.6 ms unless the process asks for 1 ms - so vblanks arrived in 15-16 ms lumps
// and frames alternated between ~31 and ~47 ms. Ask for a 1 ms timer for the whole process.
// ---------------------------------------------------------------------------------------
#if defined(_WIN32)
namespace {
struct LrTimerResolution {
  LrTimerResolution() {
    if (HMODULE winmm = LoadLibraryW(L"winmm.dll")) {
      using TimeBeginPeriod = UINT(WINAPI*)(UINT);
      if (auto begin = reinterpret_cast<TimeBeginPeriod>(GetProcAddress(winmm, "timeBeginPeriod"))) begin(1);
    }
    // Windows 11 lowers the timer resolution of processes it considers in the background
    // unless they opt out.
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | 0x4;  // 0x4: IGNORE_TIMER_RESOLUTION
    state.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
  }
} g_lr_timer_resolution;
}  // namespace
#endif

// ---------------------------------------------------------------------------------------
// Outerworld prompts, every path. The question "Do you want to enable connections with
// Outerworld services? |Yes|No" is the system text $sys_sns000 (txtres/resident/system). The
// game's script shows it on Continue / New Game as a system message window: Window.
// showMessageWindow_l -> sub_826FFFE8 -> sub_827153A0 -> sub_8270E260 builds a GuiMessageWindow
// (vtable 0x82124EF8) and its text setup sub_827CE478 gets the text already resolved in r8
// (sub_827CB1C8 counts the '|' choices in it); sub_826FDF28 then opens it (vtable +0x13C,
// sub_827CDE10), and the script sleeps until the state byte at +80 is 3 (closed) and reads the
// chosen index at +160 (Window.getSelectedItemIndex -> sub_826F87A8; 0 = first choice).
// Step 27 matched the window against the text LocalizedText::Set (sub_82729450) had resolved,
// which this text never goes through - so the question stayed. Now any message window whose
// text names the Outerworld (or is $sys_sns000 / $sys_sns001 as looked up by either lookup) is
// never opened: it is answered with its last choice ("No", "I disagree") or OK, as if the player
// had, and the script goes on as after "No". sub_827CE5F0 (the keyed variant of the setup) is
// covered the same way. After "Yes" the script would open the Terms of Use document
// ("_sns_rule" in the script message factory sub_827114F0 -> sub_827EF868(widget, 1), vtable
// 0x8212C410, +168 = 1); it is still closed unanswered in case a script opens it directly (only
// the yes/no variant: with +168 = 0 the same class is the Open-Source Software Licenses viewer
// in Settings). LR_OUTERWORLD_PROMPT=1 keeps all of them.
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_82719338);
REX_EXTERN(__imp__sub_827CB600);

namespace {
bool lr_keep_outerworld_prompt() {
  static const bool keep = [] {
    const char* v = std::getenv("LR_OUTERWORLD_PROMPT");
    return v && v[0] == '1';
  }();
  return keep;
}

void lr_close_outerworld_widget(uint8_t* base, uint32_t widget, const char* how) {
  uint32_t* state = reinterpret_cast<uint32_t*>(base + widget + 80);
  *state = __builtin_bswap32((__builtin_bswap32(*state) & 0x00FFFFFFu) | 0x03000000u);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4) REXLOG_INFO("LR Outerworld: terms of use closed, not agreed ({})", how);
}

// Reads a NUL-terminated guest string if the address is committed memory.
bool lr_guest_cstr(uint8_t* base, uint32_t address, char* out, size_t size) {
  out[0] = 0;
  if (address < 0x10000 || !size) return false;
  auto* kernel_state = REX_KERNEL_STATE();
  auto* memory = kernel_state ? kernel_state->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  rex::memory::HeapAllocationInfo info{};
  if (!heap || !heap->QueryRegionInfo(address, &info) ||
      !(info.state & rex::memory::kMemoryAllocationCommit) || !info.protect) {
    return false;
  }
  size_t limit = std::min<size_t>(size - 1, info.base_address + info.region_size - address);
  size_t n = 0;
  for (; n < limit && base[address + n]; ++n) out[n] = char(base[address + n]);
  out[n] = 0;
  return n > 0;
}

bool lr_is_outerworld_key(const char* key) {
  return std::strncmp(key, "$sys_sns", 8) == 0 || std::strncmp(key, "$sns_", 5) == 0 ||
         std::strncmp(key, "$f_sns_", 7) == 0;
}

// The resolved texts of $sys_sns000 (question) and $sys_sns001 (follow-up), as last looked up
// through either text lookup, and the message windows with an Outerworld text that have not
// been opened yet, with the choice to answer them with.
struct LrOuterworldText {
  std::string text;
};
struct LrOuterworldWindow {
  uint32_t window = 0;
  uint32_t answer = 0;
};
std::mutex g_lr_ow_mutex;
LrOuterworldText g_lr_ow_text[2];
LrOuterworldWindow g_lr_ow_windows[8];

int lr_outerworld_key(uint8_t* base, uint32_t key) {
  if (!key) return -1;
  const char* k = reinterpret_cast<const char*>(base + key);
  if (k[0] != '$' || std::strncmp(k, "$sys_sns00", 10) != 0 || k[11] != 0) return -1;
  return k[10] == '0' ? 0 : k[10] == '1' ? 1 : -1;
}

void lr_remember_outerworld_text(int kind, const char* text) {
  std::lock_guard<std::mutex> lock(g_lr_ow_mutex);
  g_lr_ow_text[kind].text.assign(text, strnlen(text, 1024));
}

// A message window's text as the game reads it: bytes with the top bit set start a two-byte
// control code (the second byte may be 0), a 0 byte otherwise ends the text. Returns false
// when the address is not committed memory.
bool lr_read_game_text(uint8_t* base, uint32_t address, std::string& out) {
  out.clear();
  if (address < 0x10000) return false;
  auto* kernel_state = REX_KERNEL_STATE();
  auto* memory = kernel_state ? kernel_state->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  rex::memory::HeapAllocationInfo info{};
  if (!heap || !heap->QueryRegionInfo(address, &info) ||
      !(info.state & rex::memory::kMemoryAllocationCommit) || !info.protect) {
    return false;
  }
  size_t limit = std::min<size_t>(4096, info.base_address + info.region_size - address);
  for (size_t i = 0; i < limit; ++i) {
    uint8_t c = base[address + i];
    if (!c) break;
    out.push_back(char(c));
    if ((c & 0x80) && i + 1 < limit) out.push_back(char(base[address + ++i]));
  }
  return true;
}

// The choice an Outerworld message window is answered with, -1 when the text is not one: the
// last choice of a question (the choices follow '|': "|Yes|No" -> 1, "No"; "|I agree|I
// disagree" -> 1) or 0 ("OK") for a notice. Matched by content - the texts of $sys_sns000 /
// $sys_sns001 as looked up, or any text naming the Outerworld - because the question reaches
// the window already resolved ("... services? |Yes|No"), whichever lookup the script used.
int lr_outerworld_answer(const std::string& text) {
  bool outerworld = text.find("Outerworld") != std::string::npos;
  if (!outerworld) {
    std::lock_guard<std::mutex> lock(g_lr_ow_mutex);
    for (const LrOuterworldText& t : g_lr_ow_text) outerworld |= !t.text.empty() && t.text == text;
  }
  if (!outerworld) return -1;
  int choices = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    uint8_t c = uint8_t(text[i]);
    if (c & 0x80) {
      ++i;
    } else if (c == '|') {
      ++choices;
    }
  }
  return choices > 0 ? choices - 1 : 0;
}

// Remembers a message window whose text is an Outerworld one (answer >= 0), forgets it
// otherwise (the address may be reused by another window).
void lr_note_message_window(uint32_t window, int answer, const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(g_lr_ow_mutex);
    LrOuterworldWindow* free_slot = nullptr;
    for (LrOuterworldWindow& w : g_lr_ow_windows) {
      if (w.window == window) w = {};
      if (!w.window && !free_slot) free_slot = &w;
    }
    if (answer >= 0) *(free_slot ? free_slot : &g_lr_ow_windows[0]) = {window, uint32_t(answer)};
  }
  if (answer < 0) return;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) {
    std::string shown;
    for (char c : text.substr(0, 90)) shown.push_back((c >= 32 && c < 127) ? c : ' ');
    REXLOG_INFO("LR Outerworld: message window {:08X} will be answered with choice {} before it opens: \"{}\"",
                window, answer, shown);
  }
}

// When the window is an Outerworld one: answers it instead of opening it, as the A button
// would with the cursor on that choice: +160 = the choice, then the window's own decide step
// sub_827CB600 (tells the window's listener at +64, then closes it the normal way: state 2,
// state 3 when the closing animation has run, released then if it frees itself). Returns true.
bool lr_answer_outerworld_window(PPCContext& ctx, uint8_t* base, uint32_t window) {
  int answer = -1;
  {
    std::lock_guard<std::mutex> lock(g_lr_ow_mutex);
    for (LrOuterworldWindow& w : g_lr_ow_windows) {
      if (w.window == window && window) {
        answer = int(w.answer);
        w = {};
      }
    }
  }
  if (answer < 0) return false;
  *reinterpret_cast<uint32_t*>(base + window + 160) = __builtin_bswap32(uint32_t(answer));
  bool listener = lr_load_be32(base, window + 64) != 0;
  if (listener) {
    uint64_t lr = ctx.lr;
    ctx.r3.u64 = window;
    ctx.lr = 0x827CEAF4;  // as from the A button handler sub_827CE8F8
    __imp__sub_827CB600(ctx, base);
    ctx.lr = lr;
  } else {
    // No listener: closed directly, as the closing step sub_827DDD88 would leave it.
    if (lr_load_be32(base, window + 72) & 0x10000000u) {
      uint64_t lr = ctx.lr;
      ctx.r3.u64 = lr_load_be32(base, 0x831203AC);
      ctx.r4.u64 = lr_load_be32(base, window + 84);
      ctx.r5.u64 = 0;
      ctx.lr = 0x827DDDD4;
      __imp__sub_82719338(ctx, base);
      ctx.lr = lr;
    }
    base[window + 80] = 3;
    base[window + 81] = 0;
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) {
    REXLOG_INFO("LR Outerworld: removed (message window {:08X} answered with choice {} without being shown{})",
                window, answer, listener ? "" : ", closed directly");
  }
  return true;
}
}  // namespace

#define LR_TEST_LOG_KEY(base, key, lookup, caller)

REX_EXTERN(__imp__sub_82729450);
REX_HOOK_RAW(sub_82729450) {
  uint32_t object = ctx.r3.u32;
  LR_TEST_LOG_KEY(base, ctx.r4.u32, "LocalizedText", uint32_t(ctx.lr));
  int kind = lr_keep_outerworld_prompt() ? -1 : lr_outerworld_key(base, ctx.r4.u32);
  uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_82729450(ctx, base);
  if (kind < 0) return;
  uint32_t text = lr_load_be32(base, object);
  if (!text) return;
  text += uint32_t(int32_t(int16_t(__builtin_bswap16(*reinterpret_cast<uint16_t*>(base + object + 10)))));
  std::string s;
  if (!lr_read_game_text(base, text, s)) return;
  lr_remember_outerworld_text(kind, s.c_str());
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4) REXLOG_INFO("LR Outerworld: $sys_sns00{} looked up (LocalizedText, from {:08X})", kind, caller);
}

// Message window text setup (sub_8270E260 -> here, r8 = the text, already resolved when it
// has choices: sub_827CB1C8 counts the '|' in it). A notice without choices gets its key
// instead ("$sys_sns001": "Change Outerworld services settings by going to the Settings
// option ...") and the setup resolves it through LocalizedText::Set into the window's text
// object at +0xE8 - matched by the key, or by the text it resolved to.
REX_EXTERN(__imp__sub_827CE478);
REX_HOOK_RAW(sub_827CE478) {
  uint32_t window = ctx.r3.u32;
  std::string text;
  if (!lr_keep_outerworld_prompt()) lr_read_game_text(base, ctx.r8.u32, text);
  __imp__sub_827CE478(ctx, base);
  if (lr_keep_outerworld_prompt()) return;
  int answer = lr_outerworld_answer(text);
  if (answer < 0 && !text.empty() && text[0] == '$') {
    if (lr_is_outerworld_key(text.c_str())) answer = 0;
    uint32_t object = window + 0xE8;
    if (uint32_t resolved = lr_load_be32(base, object)) {
      resolved += uint32_t(int32_t(int16_t(__builtin_bswap16(*reinterpret_cast<uint16_t*>(base + object + 10)))));
      std::string s;
      if (lr_read_game_text(base, resolved, s) && !s.empty()) {
        if (answer < 0) answer = lr_outerworld_answer(s);
        text = s;
      }
    }
  }
  lr_note_message_window(window, answer, text);
}

// The other message window setup (sub_8270E3A0 -> here): r8 = the text's key, r9 = an array of
// r10 choice keys, each set through LocalizedText::Set; the window's text object is at +0xE8.
REX_EXTERN(__imp__sub_827CE5F0);
REX_HOOK_RAW(sub_827CE5F0) {
  uint32_t window = ctx.r3.u32, choices = ctx.r10.u32;
  char key[48];
  bool keyed = lr_guest_cstr(base, ctx.r8.u32, key, sizeof(key)) && lr_is_outerworld_key(key);
  __imp__sub_827CE5F0(ctx, base);
  if (lr_keep_outerworld_prompt()) return;
  uint32_t object = window + 0xE8;
  uint32_t text = lr_load_be32(base, object);
  std::string s;
  if (text) {
    text += uint32_t(int32_t(int16_t(__builtin_bswap16(*reinterpret_cast<uint16_t*>(base + object + 10)))));
    lr_read_game_text(base, text, s);
  }
  int answer = lr_outerworld_answer(s);
  if (answer < 0 && keyed) answer = 0;
  if (answer >= 0) answer = choices > 0 && choices < 16 ? int(choices) - 1 : 0;
  lr_note_message_window(window, answer, s.empty() ? std::string(key) : s);
}

// GuiMessageWindow open (vtable +0x13C): plays the open sound, then the base open.
REX_EXTERN(__imp__sub_827CDE10);
REX_HOOK_RAW(sub_827CDE10) {
  if (lr_answer_outerworld_window(ctx, base, ctx.r3.u32)) return;
  __imp__sub_827CDE10(ctx, base);
}

// Base window open, for message window subclasses with their own open method.
REX_EXTERN(__imp__sub_827DDEA0);
REX_HOOK_RAW(sub_827DDEA0) {
  if (lr_answer_outerworld_window(ctx, base, ctx.r3.u32)) return;
  __imp__sub_827DDEA0(ctx, base);
}

REX_EXTERN(__imp__sub_827EF868);
REX_HOOK_RAW(sub_827EF868) {
  __imp__sub_827EF868(ctx, base);
  if (!lr_keep_outerworld_prompt() && ctx.r3.u32 && lr_load_be32(base, ctx.r3.u32 + 168)) {
    lr_close_outerworld_widget(base, ctx.r3.u32, "built");
  }
}

REX_EXTERN(__imp__sub_827EC7C8);
REX_HOOK_RAW(sub_827EC7C8) {
  // Only the document widget's vtable (0x8212C410) holds this method; checked anyway.
  if (!lr_keep_outerworld_prompt() && ctx.r3.u32 && lr_load_be32(base, ctx.r3.u32) == 0x8212C410 &&
      lr_load_be32(base, ctx.r3.u32 + 168)) {
    lr_close_outerworld_widget(base, ctx.r3.u32, "open skipped");
    return;
  }
  __imp__sub_827EC7C8(ctx, base);
}

// The Outerworld Primers (tips records in db/resident/auto_clip.wdb, each a title, a page
// text, and a picture): "Outerworld services" (atcw_fl034, $watips_f34, the two pages shown
// after the question on New Game / Continue) and the Datalog entries about posting, Outerworld
// characters, Facebook, and Twitter (atcw_fl138 - atcw_fl148, atcw_fl155). Tips windows come
// from three places, all through sub_826F9820 (-> the message factory sub_827114F0):
//  - Window.showTipsById (sub_8299C3F0): an Outerworld id is popped and the native returns as
//    if the window had been read and closed (it pushes nothing; vm+48 stays 0);
//  - the field's own tips display (sub_828AA2C0): gets -1 for the window, which it treats as
//    "not shown" (it still records the tip as seen, and the Primers list hides the Outerworld
//    category);
//  - any other caller (Window.createTipsWindowById, which hands the window to the script):
//    the window is created but never opened, and reported closed when asked.
// ---------------------------------------------------------------------------------------
namespace {
bool lr_is_outerworld_tips(const char* id) {
  if (std::strcmp(id, "atcw_fl034") == 0 || std::strcmp(id, "atcw_fl155") == 0) return true;
  if (std::strncmp(id, "atcw_fl1", 8) != 0 || id[8] < '3' || id[8] > '4' || id[9] < '0' || id[9] > '9' || id[10]) {
    return false;
  }
  int n = (id[8] - '0') * 10 + (id[9] - '0');
  return n >= 38 && n <= 48;
}

void lr_log_outerworld_tips(const char* id, const char* how) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) REXLOG_INFO("LR Outerworld: removed (Primer {} {})", id, how);
}
}  // namespace

REX_EXTERN(__imp__sub_8299C3F0);
REX_HOOK_RAW(sub_8299C3F0) {
  uint32_t vm = ctx.r3.u32;
  if (!lr_keep_outerworld_prompt() && vm && !lr_load_be32(base, vm + 48)) {
    uint32_t sp = lr_load_be32(base, vm + 2392);
    char id[32];
    if (lr_guest_cstr(base, lr_load_be32(base, sp + 4), id, sizeof(id)) && lr_is_outerworld_tips(id)) {
      *reinterpret_cast<uint32_t*>(base + vm + 2392) = __builtin_bswap32(sp + 8);
      lr_log_outerworld_tips(id, "not shown by the script");
      return;
    }
  }
  __imp__sub_8299C3F0(ctx, base);
}

// Windows created for an Outerworld Primer by any other caller (Window.createTipsWindowById
// hands the window to the script, which opens it and waits for it to close): never opened
// (sub_826FDF28 skips them) and reported closed (sub_826F6E80), so the script releases it and
// goes on as if it had been read.
std::mutex g_lr_ow_tips_mutex;
std::vector<uint32_t> g_lr_ow_tips_windows;

bool lr_is_outerworld_tips_window(uint32_t window) {
  std::lock_guard<std::mutex> lock(g_lr_ow_tips_mutex);
  return std::find(g_lr_ow_tips_windows.begin(), g_lr_ow_tips_windows.end(), window) !=
         g_lr_ow_tips_windows.end();
}

REX_EXTERN(__imp__sub_826F9820);
REX_HOOK_RAW(sub_826F9820) {
  char id[32];
  bool outerworld = !lr_keep_outerworld_prompt() && lr_guest_cstr(base, ctx.r3.u32, id, sizeof(id)) &&
                    lr_is_outerworld_tips(id);
  uint32_t caller = uint32_t(ctx.lr);
  if (outerworld && caller == 0x828AA318) {
    ctx.r3.u64 = 0xFFFFFFFFu;
    lr_log_outerworld_tips(id, "not shown by the field");
    return;
  }
  __imp__sub_826F9820(ctx, base);
  if (outerworld && int32_t(ctx.r3.u32) >= 0) {
    {
      std::lock_guard<std::mutex> lock(g_lr_ow_tips_mutex);
      if (g_lr_ow_tips_windows.size() >= 16) g_lr_ow_tips_windows.erase(g_lr_ow_tips_windows.begin());
      g_lr_ow_tips_windows.push_back(ctx.r3.u32);
    }
    lr_log_outerworld_tips(id, fmt::format("window {} never opened (from {:08X})", ctx.r3.u32, caller).c_str());
  }
}

// Window open by id (r3).
REX_EXTERN(__imp__sub_826FDF28);
REX_HOOK_RAW(sub_826FDF28) {
  if (lr_is_outerworld_tips_window(ctx.r3.u32)) return;
  __imp__sub_826FDF28(ctx, base);
}

// "Has the window closed?" by id (r3): nonzero when it has.
REX_EXTERN(__imp__sub_826F6E80);
REX_HOOK_RAW(sub_826F6E80) {
  if (lr_is_outerworld_tips_window(ctx.r3.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_826F6E80(ctx, base);
}

// ---------------------------------------------------------------------------------------
// Outerworld, stripped. The hooks above never saw the question after the title screen: the
// event script asks it through the Window.ask* natives (Window.askChoice = sub_8299A8C0,
// script VM stack at vm+2392, 8-byte slots: u16 type, u32 value at +4), whose window builder
// sub_82866540 resolves "$key" texts with sub_826FE158, not with LocalizedText::Set. Here:
//  - any Window.ask* call whose text is an Outerworld key ($sys_sns*, $sns_*, $f_sns_*)
//    returns at once with the answer that keeps Outerworld off (the second choice - "No",
//    "I disagree" - or "OK" for a notice), the way the native returns when no window is
//    needed: arguments popped, the answer pushed as an int. No window is built;
//  - Settings: "Outerworld Services", "Outerworld Characters", "Link with Facebook", and
//    "Link with Twitter" (config ids 10, 12, 13, 14; see lr_config_menu.inc) are not listed;
//  - Rewards Barter Shop: the "Outerworld Rewards" row ($shop_96) stays. It was skipped
//    (not added through sub_8283BEA8), but the shop is built for its three rows (ids 0-2,
//    sub_82340E40) and crashed in use without the third;
//  - Primers: the "The Outerworld" category (14, the last one sub_82780E30 adds through
//    sub_82839DC8, followed by its "new" mark sub_828399A0);
//  - help texts that mention Outerworld lose that part.
// The game's own Outerworld switch stays off (Settings never offers it), so nothing connects.
// LR_OUTERWORLD_PROMPT=1 keeps everything.
// ---------------------------------------------------------------------------------------
namespace {
// For a Window.ask* native with `args` arguments: if one of them is an Outerworld text,
// answers without asking. Only a fresh call (vm+2404 bit 0x80 marks a resumed wait).
bool lr_strip_outerworld_ask(PPCContext& ctx, uint8_t* base, uint32_t args, uint32_t answer,
                             const char* native) {
  if (lr_keep_outerworld_prompt()) return false;
  uint32_t vm = ctx.r3.u32;
  if (!vm || (lr_load_be32(base, vm + 2404) & 0x80)) return false;
  uint32_t sp = lr_load_be32(base, vm + 2392);
  char key[48];
  bool found = false;
  for (uint32_t i = 0; i < args && !found; ++i) {
    found = lr_guest_cstr(base, lr_load_be32(base, sp + 8 * i + 4), key, sizeof(key)) &&
            lr_is_outerworld_key(key);
  }
  if (!found) return false;
  sp += 8 * args - 8;
  *reinterpret_cast<uint16_t*>(base + sp) = __builtin_bswap16(1);
  *reinterpret_cast<uint32_t*>(base + sp + 4) = __builtin_bswap32(answer);
  *reinterpret_cast<uint32_t*>(base + vm + 2392) = __builtin_bswap32(sp);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 16) {
    REXLOG_INFO("LR Outerworld: removed ({} {} not shown, answered {})", native, key, answer);
  }
  return true;
}

// Texts edited in place: help texts that mention Outerworld lose that part, and the save
// screens' storage device texts ("Selected Storage Device: Xbox 360 Hard Drive", the
// "Select storage device" hint - there is only the one device here) are emptied (cut =
// nullptr).
struct LrTextCut {
  const char* key;
  const char* cut;
  const char* with;
  bool outerworld;
};
constexpr LrTextCut kTextCuts[] = {
    {"$a_b03_1_xb", ", special items from the Outerworld,", "", true},
    {"$a_b03_1_ps", ", special items from the Outerworld,", "", true},
    {"$f_menu_73h", ", Outerworld rewards, and", " and", true},
    {"$f_menu_73h_x", ", Outerworld rewards, and", " and", true},
    {"$xsave_16", nullptr, nullptr, false},
    {"$xsave_18", nullptr, nullptr, false},
    {"$xsave_20", nullptr, nullptr, false},
    {"$xsave_21", nullptr, nullptr, false},
};

void lr_cut_outerworld_text(uint8_t* base, const char* key, uint32_t text) {
  if (!text) return;
  for (const LrTextCut& c : kTextCuts) {
    if (std::strcmp(key, c.key) != 0) continue;
    if (c.outerworld && lr_keep_outerworld_prompt()) return;
    char* s = reinterpret_cast<char*>(base + text);
    if (!c.cut) {
      static std::atomic<int> logged{0};
      if (s[0] && logged.fetch_add(1) < 4) REXLOG_INFO("LR text: {} emptied (storage device)", key);
      s[0] = 0;
      s[1] = 0;
      return;
    }
    // The game's texts end with two NULs (control codes may hold single ones).
    size_t length = 0;
    while (length < 4094 && (s[length] || s[length + 1])) ++length;
    size_t found = std::string_view(s, length).find(c.cut);
    if (found == std::string_view::npos) return;
    char* at = s + found;
    size_t cut = std::strlen(c.cut), with = std::strlen(c.with);
    std::memcpy(at, c.with, with);
    std::memmove(at + with, at + cut, length - size_t(at - s) - cut + 2);
    return;
  }
}
}  // namespace

// Window.askOk (1 argument), askOkCancel (1), askChoice (2), askChoiceByList (2),
// askChoiceWithInitNo (3), askChoiceByListWithInitNo (3).
REX_EXTERN(__imp__sub_8299A2F8);
REX_HOOK_RAW(sub_8299A2F8) {
  if (lr_strip_outerworld_ask(ctx, base, 1, 0, "askOk")) return;
  __imp__sub_8299A2F8(ctx, base);
}
REX_EXTERN(__imp__sub_8299A470);
REX_HOOK_RAW(sub_8299A470) {
  if (lr_strip_outerworld_ask(ctx, base, 1, 1, "askOkCancel")) return;
  __imp__sub_8299A470(ctx, base);
}
REX_EXTERN(__imp__sub_8299A8C0);
REX_HOOK_RAW(sub_8299A8C0) {
  if (lr_strip_outerworld_ask(ctx, base, 2, 1, "askChoice")) return;
  __imp__sub_8299A8C0(ctx, base);
}
REX_EXTERN(__imp__sub_8299A5D0);
REX_HOOK_RAW(sub_8299A5D0) {
  if (lr_strip_outerworld_ask(ctx, base, 2, 1, "askChoiceByList")) return;
  __imp__sub_8299A5D0(ctx, base);
}
REX_EXTERN(__imp__sub_8299AA38);
REX_HOOK_RAW(sub_8299AA38) {
  if (lr_strip_outerworld_ask(ctx, base, 3, 1, "askChoiceWithInitNo")) return;
  __imp__sub_8299AA38(ctx, base);
}
REX_EXTERN(__imp__sub_8299A748);
REX_HOOK_RAW(sub_8299A748) {
  if (lr_strip_outerworld_ask(ctx, base, 3, 1, "askChoiceByListWithInitNo")) return;
  __imp__sub_8299A748(ctx, base);
}

// "$key" -> text for message windows (returns the text table's string). The texts of
// $sys_sns000 / $sys_sns001 are remembered for the message window match above.
REX_EXTERN(__imp__sub_826FE158);
REX_HOOK_RAW(sub_826FE158) {
  char key[32];
  bool have_key = ctx.r3.u32 && lr_guest_cstr(base, ctx.r3.u32, key, sizeof(key));
  uint32_t caller = uint32_t(ctx.lr);
  LR_TEST_LOG_KEY(base, ctx.r3.u32, "table", caller);
  __imp__sub_826FE158(ctx, base);
  if (!have_key) return;
  lr_cut_outerworld_text(base, key, ctx.r3.u32);
  lr_title_quit_label(base, ctx.r3.u32);
  if (lr_keep_outerworld_prompt() || !ctx.r3.u32 || std::strncmp(key, "$sys_sns00", 10) != 0 ||
      (key[10] != '0' && key[10] != '1') || key[11]) {
    return;
  }
  int kind = key[10] - '0';
  std::string text;
  if (!lr_read_game_text(base, ctx.r3.u32, text)) return;
  lr_remember_outerworld_text(kind, text.c_str());
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4) REXLOG_INFO("LR Outerworld: {} looked up (text table, from {:08X})", key, caller);
}

// The heap copies LocalizedText::Set gets (sub_8270CA90) are cut in lr_config_menu.inc's
// hook of that function.

// Primers categories: no "The Outerworld" (14), nor the "new" mark set right after it.
thread_local bool t_lr_primer_skipped = false;
REX_EXTERN(__imp__sub_82839DC8);
REX_HOOK_RAW(sub_82839DC8) {
  char key[20];
  if (!lr_keep_outerworld_prompt() && ctx.r5.u32 == 14 &&
      lr_guest_cstr(base, ctx.r4.u32, key, sizeof(key)) && std::strcmp(key, "$m_atcl_cat14") == 0) {
    t_lr_primer_skipped = true;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) REXLOG_INFO("LR Outerworld: removed (Primers category \"The Outerworld\")");
    return;
  }
  __imp__sub_82839DC8(ctx, base);
}
REX_EXTERN(__imp__sub_828399A0);
REX_HOOK_RAW(sub_828399A0) {
  if (t_lr_primer_skipped) {
    t_lr_primer_skipped = false;
    return;
  }
  __imp__sub_828399A0(ctx, base);
}

// ---------------------------------------------------------------------------------------
// Controller input, one call at a time. The game polls the pad from jobs that can run on
// several threads at once (sub_829E83F8 hands the same batch to the workers and the main
// thread), and the runtime's input system rebuilds its device list on every call without a
// lock, so two polls at the same moment corrupt the heap (seen as an abort in
// InputSystem::RefreshDevices in a long test run). On the 360 these calls are thread safe.
// Every call reaches the runtime through these three wrappers (XInputGetState,
// XInputGetCapabilities + XInputSetState, XInputGetKeystroke).
// ---------------------------------------------------------------------------------------
namespace {
std::mutex g_lr_input_mutex;

std::unique_lock<std::mutex> lr_input_lock() {
  std::unique_lock<std::mutex> lock(g_lr_input_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    static std::atomic<uint32_t> collisions{0};
    uint32_t n = collisions.fetch_add(1) + 1;
    if (n == 1 || n == 100 || n == 10000) {
      REXLOG_INFO("LR input: two game threads read the controller at the same time (seen {}x); "
                  "the calls now take turns", n);
    }
    lock.lock();
  }
  return lock;
}
}  // namespace

std::atomic<bool> g_lr_input_blocked{false};
std::atomic<bool> g_lr_screenshot_request{false};  // lightningreturns_app.h takes one when set
extern std::atomic<int> g_lr_controls_open;  // lr_keyboard_menu.inc: the Gamepad Controls picture
extern std::function<void(bool)> g_lr_controls_ui;
#if defined(__ANDROID__)
void lr_touch_merge(uint8_t* base, uint32_t state, PPCContext& ctx);  // below
#endif
REX_EXTERN(__imp__sub_82D329C0);
REX_HOOK_RAW(sub_82D329C0) {
  auto lock = lr_input_lock();
  const uint32_t lr_state = ctx.r4.u32;
  const uint32_t lr_user = ctx.r3.u32;
  const PPCContext lr_saved = ctx;
  __imp__sub_82D329C0(ctx, base);
  // Controller 1 not connected, but another one is: the game reads that one. The input system
  // keeps each device's slot, and Android announces a USB controller twice when the game starts
  // (its own controller path, then SDL's USB driver, which then drops the first): the controller
  // that remains sat in slot 2, and the game - reading slot 1 only - saw nothing until it was
  // plugged in again.
  if ((lr_user & 0xFF) == 0 && lr_state && ctx.r3.u32 != 0) {
    for (uint32_t other = 1; other < 4; ++other) {
      PPCContext c = lr_saved;
      c.r3.u64 = other;
      c.r4.u64 = lr_state;
      __imp__sub_82D329C0(c, base);
      if (c.r3.u32 == 0) {
        ctx.r3.u64 = 0;
        static std::atomic<uint32_t> logged{~0u};
        if (logged.exchange(other) != other)
          REXLOG_INFO("LR input: no controller in slot 1; reading the one in slot {}", other + 1);
        break;
      }
    }
  }
#if defined(__ANDROID__)
  // On-screen controls and the Ark menu (android/project LrTouch.java).
  if ((lr_user & 0xFF) == 0 && lr_state) lr_touch_merge(base, lr_state, ctx);
#else
  (void)lr_user;
#endif
  // Settings > Keyboard Settings is waiting for a key (lr_keyboard_menu.inc): the game sees no
  // buttons until it is released.
  if (g_lr_input_blocked.load(std::memory_order_relaxed) && ctx.r3.u32 == 0 && lr_state) {
    std::memset(base + lr_state + 4, 0, 12);
  }
  // The Gamepad Controls picture is up (drawn by the app): A, B, Start, or Back closes it, and the
  // game sees no buttons until the one that opened it and the one that closed it are released.
  if (int open = g_lr_controls_open.load(std::memory_order_relaxed); open && ctx.r3.u32 == 0 && lr_state) {
    const uint16_t buttons = uint16_t(base[lr_state + 4] << 8 | base[lr_state + 5]);
    if (open == 1 && !buttons) g_lr_controls_open.store(2);
    if (open == 2 && (buttons & 0x3030)) g_lr_controls_open.store(3);
    if (open == 2 && (buttons & 0x3030) && g_lr_controls_ui) g_lr_controls_ui(false);
    if (open == 3 && !buttons) {
      g_lr_controls_open.store(0);
      REXLOG_INFO("LR controls: Gamepad Controls closed");
    }
    std::memset(base + lr_state + 4, 0, 12);
  }
}

REX_EXTERN(__imp__sub_82D329D0);
REX_HOOK_RAW(sub_82D329D0) {
  auto lock = lr_input_lock();
  __imp__sub_82D329D0(ctx, base);
}

// Keyboard mode (PC, no controller connected; set by the app): the runtime's keyboard device
// reports button state but no keystrokes, and screens that read keystrokes (the first-start
// "Press any button" notice, some menus) would ignore the keyboard. When the runtime has none
// queued, keystrokes are made here from the changes in the pad state (VK_PAD_* codes, as the
// controller driver produces them).
std::atomic<bool> g_lr_keyboard_mode{false};

namespace {
struct LrKeystrokeSynth {
  uint32_t last = 0;  // buttons (low 16 bits) + trigger / stick directions (bits 16+)
  std::deque<std::pair<uint16_t, uint16_t>> queue;  // (VK_PAD_*, flags)
};
LrKeystrokeSynth g_lr_keystrokes;

uint16_t lr_load_be16(const uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, base + address, 2);
  return __builtin_bswap16(v);
}

uint32_t lr_pad_bits(uint8_t* base, uint32_t state) {
  // X_INPUT_STATE: packet number, then X_INPUT_GAMEPAD (big endian): buttons u16, LT u8, RT u8,
  // LX, LY, RX, RY s16.
  uint16_t buttons = lr_load_be16(base, state + 4);
  uint8_t lt = base[state + 6], rt = base[state + 7];
  auto axis = [&](uint32_t o) { return int16_t(lr_load_be16(base, state + o)); };
  int16_t lx = axis(8), ly = axis(10), rx = axis(12), ry = axis(14);
  constexpr int kDead = 16000;
  uint32_t bits = buttons;
  if (lt > 0x80) bits |= 1u << 16;
  if (rt > 0x80) bits |= 1u << 17;
  if (ly > kDead) bits |= 1u << 18;
  if (ly < -kDead) bits |= 1u << 19;
  if (lx < -kDead) bits |= 1u << 20;
  if (lx > kDead) bits |= 1u << 21;
  if (ry > kDead) bits |= 1u << 22;
  if (ry < -kDead) bits |= 1u << 23;
  if (rx < -kDead) bits |= 1u << 24;
  if (rx > kDead) bits |= 1u << 25;
  return bits;
}

uint16_t lr_pad_vk(uint32_t bit) {
  switch (bit) {
    case 0: return 0x5810;   // DPAD_UP
    case 1: return 0x5811;   // DPAD_DOWN
    case 2: return 0x5812;   // DPAD_LEFT
    case 3: return 0x5813;   // DPAD_RIGHT
    case 4: return 0x5814;   // START
    case 5: return 0x5815;   // BACK
    case 6: return 0x5816;   // LTHUMB_PRESS
    case 7: return 0x5817;   // RTHUMB_PRESS
    case 8: return 0x5805;   // LSHOULDER
    case 9: return 0x5804;   // RSHOULDER
    case 12: return 0x5800;  // A
    case 13: return 0x5801;  // B
    case 14: return 0x5802;  // X
    case 15: return 0x5803;  // Y
    case 16: return 0x5806;  // LTRIGGER
    case 17: return 0x5807;  // RTRIGGER
    case 18: return 0x5820;  // LTHUMB_UP
    case 19: return 0x5821;  // LTHUMB_DOWN
    case 20: return 0x5823;  // LTHUMB_LEFT
    case 21: return 0x5822;  // LTHUMB_RIGHT
    case 22: return 0x5830;  // RTHUMB_UP
    case 23: return 0x5831;  // RTHUMB_DOWN
    case 24: return 0x5833;  // RTHUMB_LEFT
    case 25: return 0x5832;  // RTHUMB_RIGHT
    default: return 0;
  }
}
}  // namespace

#if defined(__ANDROID__)
// ---------------------------------------------------------------------------------------
// Android touch layer (android/project/.../LrTouch.java): the on-screen controls are merged
// into controller 1's state here, so the game sees one pad whether or not a controller is
// connected (with none, the on-screen pad is reported as connected). While the Ark menu is open
// the game sees no buttons, and controller presses go to the menu instead.
// ---------------------------------------------------------------------------------------
namespace {
struct LrTouchPad {
  bool active = false;
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint32_t seq = 0;
};
std::mutex g_lr_touch_mutex;
LrTouchPad g_lr_touch;
std::atomic<bool> g_lr_touch_menu{false};
std::atomic<uint32_t> g_lr_touch_menu_edges{0};
uint16_t g_lr_touch_menu_last = 0;  // under the input lock
std::atomic<bool> g_lr_touch_physical{false};
LrKeystrokeSynth g_lr_touch_keystrokes;

LrTouchPad lr_touch_get() {
  std::lock_guard<std::mutex> lock(g_lr_touch_mutex);
  return g_lr_touch;
}

void lr_store_be16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }

// Direction bits as lr_pad_bits makes them, from the touch pad alone.
uint32_t lr_touch_bits(const LrTouchPad& t) {
  constexpr int kDead = 16000;
  uint32_t bits = t.buttons;
  if (t.lt > 0x80) bits |= 1u << 16;
  if (t.rt > 0x80) bits |= 1u << 17;
  if (t.ly > kDead) bits |= 1u << 18;
  if (t.ly < -kDead) bits |= 1u << 19;
  if (t.lx < -kDead) bits |= 1u << 20;
  if (t.lx > kDead) bits |= 1u << 21;
  if (t.ry > kDead) bits |= 1u << 22;
  if (t.ry < -kDead) bits |= 1u << 23;
  if (t.rx < -kDead) bits |= 1u << 24;
  if (t.rx > kDead) bits |= 1u << 25;
  return bits;
}
}  // namespace

void lr_touch_merge(uint8_t* base, uint32_t state, PPCContext& ctx) {
  uint8_t* st = base + state;
  const bool connected = ctx.r3.u32 == 0;
  if (connected) {
    uint16_t b = lr_load_be16(base, state + 4);
    auto axis = [&](uint32_t o) { return int(int16_t(lr_load_be16(base, state + o))); };
    if (b || st[6] > 0x40 || st[7] > 0x40 || std::abs(axis(8)) > 12000 || std::abs(axis(10)) > 12000 ||
        std::abs(axis(12)) > 12000 || std::abs(axis(14)) > 12000)
      g_lr_touch_physical.store(true, std::memory_order_relaxed);
  }
  if (g_lr_touch_menu.load(std::memory_order_relaxed)) {
    uint16_t b = connected ? lr_load_be16(base, state + 4) : 0;
    // Stick directions count as d-pad presses in the menu.
    int16_t ly = connected ? int16_t(lr_load_be16(base, state + 10)) : 0;
    if (ly > 20000) b |= 0x0001;
    if (ly < -20000) b |= 0x0002;
    g_lr_touch_menu_edges.fetch_or(b & ~g_lr_touch_menu_last, std::memory_order_relaxed);
    g_lr_touch_menu_last = b;
    if (connected) std::memset(st + 4, 0, 12);
    return;
  }
  g_lr_touch_menu_last = 0;
  LrTouchPad t = lr_touch_get();
  if (!t.active) return;
  if (!connected) {
    std::memset(st, 0, 16);
    ctx.r3.u64 = 0;
  }
  lr_store_be16(st + 4, uint16_t(lr_load_be16(base, state + 4) | t.buttons));
  st[6] = std::max(st[6], t.lt);
  st[7] = std::max(st[7], t.rt);
  auto put_axis = [&](uint32_t o, int16_t v) { if (v) lr_store_be16(st + o, uint16_t(v)); };
  put_axis(8, t.lx);
  put_axis(10, t.ly);
  put_axis(12, t.rx);
  put_axis(14, t.ry);
  uint32_t packet = lr_load_be32(base, state) + (t.seq << 16);
  uint32_t be = __builtin_bswap32(packet);
  std::memcpy(st, &be, 4);
}

void LrTouchSetPad(bool active, uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly,
                   int16_t rx, int16_t ry) {
  std::lock_guard<std::mutex> lock(g_lr_touch_mutex);
  LrTouchPad& t = g_lr_touch;
  if (t.active == active && t.buttons == buttons && t.lt == lt && t.rt == rt && t.lx == lx &&
      t.ly == ly && t.rx == rx && t.ry == ry)
    return;
  t.active = active;
  t.buttons = buttons;
  t.lt = lt;
  t.rt = rt;
  t.lx = lx;
  t.ly = ly;
  t.rx = rx;
  t.ry = ry;
  ++t.seq;
}

void LrTouchSetMenuOpen(bool open) {
  g_lr_touch_menu.store(open, std::memory_order_relaxed);
  g_lr_touch_menu_edges.store(0, std::memory_order_relaxed);
}
uint32_t LrTouchTakeMenuButtons() { return g_lr_touch_menu_edges.exchange(0, std::memory_order_relaxed); }
bool LrTouchTakePhysicalActivity() { return g_lr_touch_physical.exchange(false, std::memory_order_relaxed); }
#endif

REX_EXTERN(__imp__sub_82D32A98);
REX_HOOK_RAW(sub_82D32A98) {
  auto lock = lr_input_lock();
  uint32_t user = ctx.r3.u32, keystroke = ctx.r5.u32;
  PPCContext saved = ctx;
  __imp__sub_82D32A98(ctx, base);
  constexpr uint32_t kErrorEmpty = 0x10D2;
#if defined(__ANDROID__)
  if (g_lr_touch_menu.load(std::memory_order_relaxed)) {
    ctx.r3.u64 = kErrorEmpty;  // the Ark menu is open: the game gets no keystrokes
    return;
  }
  // On-screen controls: keystrokes from their changes (the controller's own come from the
  // runtime), for the screens that read keystrokes.
  if (ctx.r3.u32 == kErrorEmpty && keystroke && ((user & 0xFF) == 0 || (user & 0xFF) == 0xFF)) {
    LrTouchPad t = lr_touch_get();
    auto& synth = g_lr_touch_keystrokes;
    uint32_t now = t.active ? lr_touch_bits(t) : 0;
    uint32_t changed = now ^ synth.last;
    for (uint32_t bit = 0; bit < 26; ++bit) {
      if (!(changed & (1u << bit))) continue;
      if (uint16_t vk = lr_pad_vk(bit)) synth.queue.emplace_back(vk, (now & (1u << bit)) ? 0x0001 : 0x0002);
    }
    synth.last = now;
    if (!synth.queue.empty()) {
      auto [vk, flags] = synth.queue.front();
      synth.queue.pop_front();
      uint16_t be_vk = __builtin_bswap16(vk), be_flags = __builtin_bswap16(flags), zero = 0;
      std::memcpy(base + keystroke, &be_vk, 2);
      std::memcpy(base + keystroke + 2, &zero, 2);
      std::memcpy(base + keystroke + 4, &be_flags, 2);
      base[keystroke + 6] = 0;
      base[keystroke + 7] = 0;
      ctx.r3.u64 = 0;
      return;
    }
  }
#endif
  if (!g_lr_keyboard_mode.load(std::memory_order_relaxed) || ctx.r3.u32 != kErrorEmpty ||
      !keystroke || ((user & 0xFF) != 0 && (user & 0xFF) != 0xFF)) {
    return;
  }
  // Current pad state of user 0 through the original XInputGetState wrapper, on scratch guest
  // stack well below the caller's frame (and its red zone).
  PPCContext call = saved;
  uint32_t state = saved.r1.u32 - 0xC00;
  call.r1.u64 = saved.r1.u32 - 0x1000;
  call.r3.u64 = 0;
  call.r4.u64 = state;
  std::memset(base + state, 0, 16);
  __imp__sub_82D329C0(call, base);
  auto& synth = g_lr_keystrokes;
  if (call.r3.u32 == 0) {
    uint32_t now = lr_pad_bits(base, state);
    uint32_t changed = now ^ synth.last;
    for (uint32_t bit = 0; bit < 26; ++bit) {
      if (!(changed & (1u << bit))) continue;
      if (uint16_t vk = lr_pad_vk(bit)) {
        synth.queue.emplace_back(vk, (now & (1u << bit)) ? 0x0001 : 0x0002);  // KEYDOWN / KEYUP
      }
    }
    synth.last = now;
  }
  if (synth.queue.empty()) return;
  auto [vk, flags] = synth.queue.front();
  synth.queue.pop_front();
  // X_INPUT_KEYSTROKE (big endian): VirtualKey, Unicode, Flags (u16), UserIndex, HidCode (u8).
  uint16_t be_vk = __builtin_bswap16(vk), be_flags = __builtin_bswap16(flags), zero = 0;
  std::memcpy(base + keystroke, &be_vk, 2);
  std::memcpy(base + keystroke + 2, &zero, 2);
  std::memcpy(base + keystroke + 4, &be_flags, 2);
  base[keystroke + 6] = 0;
  base[keystroke + 7] = 0;
  ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------------------
// Frame-pacing fence deadlock (the freeze on Continue / loading, and in gameplay).
// sub_82A82278 takes the spinlock at [0x8313F72C]+6984 and, while holding it, calls
// sub_829CA1B8 to insert a GPU fence. That function takes one of the device's fence slots
// (bitmap at device+24416) and, when none is free, spins until one is (flags bit 2 clear).
// Slots are freed only by sub_82A81EB8 (called from the periodic callback sub_82A81F90), which
// first takes the same spinlock - so once the slots run out the game deadlocks: the main
// thread spins on the slots holding the lock, the callback spins on the lock. The slots run
// out because sub_82A82278 stores each new fence into +6976 over the previous one while the
// callback has not caught up (loading screens, slow frames), leaking a slot every time.
// Here: while a fence from sub_82A82278 is still pending it is kept instead of taking another
// slot, and a new one is taken in the game's own no-wait mode (returns 0 = no fence when the
// slots are full) instead of spinning with the lock held.
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_829CA1B8);
REX_HOOK_RAW(sub_829CA1B8) {
  static const bool off = [] {
    const char* v = std::getenv("LR_FENCE_FIX");
    return v && v[0] == '0';
  }();
  if (uint32_t(ctx.lr) == 0x82A82278 + 0x78) {  // the call in sub_82A82278 (returns to 82A822F0)
    uint32_t pacing = lr_load_be32(base, 0x8313F72C);
    uint64_t pending = pacing ? __builtin_bswap64(*reinterpret_cast<uint64_t*>(base + pacing + 6976)) : 0;
    if (pending) {
      // The previous frame's fence was not given back yet (that frame finished late).
      static std::atomic<uint32_t> late{0};
      uint32_t n = ++late;
      if ((n & (n - 1)) == 0) {
        if (off) {
          REXLOG_INFO("LR fence: {} GPU fence slot(s) leaked by late frames (LR_FENCE_FIX=0); the game "
                      "freezes when all 32 are gone", n);
        } else {
          REXLOG_INFO("LR fence: a frame finished late {}x; its GPU fence slot was reused instead of "
                      "leaked", n);
        }
      }
      if (!off) {
        ctx.r3.u64 = pending;
        return;
      }
    }
    if (!off) ctx.r8.u64 |= 2;
  }
  __imp__sub_829CA1B8(ctx, base);
}

// ---------------------------------------------------------------------------------------
// Settings the game's own Config menu changes (lr_config_menu.cpp): voice language and UI
// style. Both take effect immediately.
//
// Voice: the Japanese voices are the update archive (priority 0, udp/white_patch_u). The
// game's archive table [0x830EDE64] holds up to 73 archives of 288 bytes: +32 file list,
// +48 enabled flag (the file lookup sub_82644A88 skips archives with 0; sub_82644990 is the
// game's own setter, used for its priority-5 archives), top 6 bits of +52 priority. With the
// flag off every voice comes from the English base archives again, from the next line that
// loads. The flag is set on the main thread once per frame when the choice changed.
// UI style: the renderer plugin switches between the PS3 and 360 UI texture sets.
// Both choices are kept in Documents\lightningreturns (voice.txt, ui_style.txt).
// ---------------------------------------------------------------------------------------
#include <fstream>

namespace {
std::filesystem::path g_lr_settings_dir;
std::atomic<int> g_lr_voice_jp{1}, g_lr_voice_applied{-1}, g_lr_voice_archives{-1};
std::atomic<int> g_lr_text_jp{0};
std::atomic<int> g_lr_aa{0}, g_lr_aa_applied{-1};
std::atomic<int> g_lr_cheats{0};  // Settings > Cheats: bit 0 free Teleport / Chronostasis, bit 1 freeze clock  // Anti-Aliasing: 0 off, 1 FXAA, 2 FXAA extreme  // Japanese on-screen text (the Steam release's txtres_jp, see below)
std::atomic<int> g_lr_ui_ps3{0};
// Button Prompts "Auto": follow the controller in use (lightningreturns_app.h tells
// LrUiStyleForController); -1 = no controller seen yet.
std::atomic<int> g_lr_ui_auto{1}, g_lr_pad_ps3{-1};
std::atomic<int> g_lr_confirm_b{0};  // 1: B confirms, A cancels (Settings > Confirm Button)
std::atomic<int> g_lr_fps60{0};  // 0: 30 fps; 1: 60 fps except cutscenes (the PC "Dynamic"); 2: 60 everywhere

std::string lr_read_setting(const char* file) {
  std::string value;
  if (g_lr_settings_dir.empty()) return value;
  std::ifstream in(g_lr_settings_dir / file);
  std::getline(in, value);
  return value;
}

void lr_write_setting(const char* file, const char* value) {
  if (g_lr_settings_dir.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(g_lr_settings_dir, ec);
  std::ofstream(g_lr_settings_dir / file) << value << "\n";
}

template <typename Fn>
Fn lr_plugin_function(const char* name) {
#if defined(_WIN32)
  HMODULE module = GetModuleHandleW(L"rexgpu-xenos.dll");
  return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
#else
  void* module = dlopen("librexgpu-xenos.so", RTLD_NOW | RTLD_NOLOAD);
  return module ? reinterpret_cast<Fn>(dlsym(module, name)) : nullptr;
#endif
}

void lr_set_ui_style(int ps3) {
  g_lr_ui_ps3.store(ps3 ? 1 : 0);
  auto set = lr_plugin_function<int (*)(int)>("LrSetUiStyle");
  if (set) set(ps3 ? 1 : 0);
  else REXLOG_WARN("UI style: the renderer plugin has no LrSetUiStyle");
}

// Anti-aliasing: the renderer's FXAA pass (lr_set_post_effect), switched from the next frame;
// called every frame until the renderer is up and has taken the choice.
void lr_apply_aa() {
  int want = g_lr_aa.load(std::memory_order_relaxed);
  if (want == g_lr_aa_applied.load(std::memory_order_relaxed)) return;
  static int32_t (*set)(int32_t) = nullptr;
  if (!set) set = lr_plugin_function<int32_t (*)(int32_t)>("lr_set_post_effect");
  if (!set || !set(want)) return;
  g_lr_aa_applied.store(want, std::memory_order_relaxed);
  REXLOG_INFO("LR anti-aliasing: {}", want == 2 ? "FXAA extreme" : want == 1 ? "FXAA" : "off");
}

void lr_apply_voice(uint8_t* base) {
  int want = g_lr_voice_jp.load(std::memory_order_relaxed);
  if (want == g_lr_voice_applied.load(std::memory_order_relaxed)) return;
  uint32_t table = lr_load_be32(base, 0x830EDE64);
  if (!table) return;
  int found = 0;
  for (uint32_t i = 0; i < 73; ++i) {
    uint32_t entry = table + i * 288;
    if (!lr_load_be32(base, entry + 32) || (lr_load_be32(base, entry + 52) >> 26) != 0) continue;
    *reinterpret_cast<uint32_t*>(base + entry + 48) = __builtin_bswap32(want ? 1u : 0u);
    ++found;
  }
  g_lr_voice_archives.store(found, std::memory_order_relaxed);
  if (!found) return;  // not registered yet, or the Japanese voices are not installed
  g_lr_voice_applied.store(want, std::memory_order_relaxed);
  REXLOG_INFO("LR voice: {}", want ? "Japanese" : "English");
}
}  // namespace

// ---------------------------------------------------------------------------------------
// Japanese on-screen text. The game asks its archives for a file by a 64-bit key (file type
// << 32 | file code) in sub_82644A88(archives, key, path out, path size, location out); a
// text file's code holds its language in one hex digit (txtres_us 1, fr 5, sp 6, jp 0; the
// name comes back from the digit: table 0x82EEA42C, sub_82652978). The 360 release has no
// txtres_jp; LR_JP_Voice.ps1 puts the Steam release's (same codes, same big-endian format)
// into the update archive and lists "<English key> <Japanese key>" in jp_text.txt
// (LR_JP_TEXT_MAP). With Japanese text on, a lookup of an English text key asks for the
// Japanese one instead. The update archive is the voice archive: when the voices are English
// it is off, so it is turned on for that one lookup, under the archive list's own lock
// (sub_82626110 / sub_82626170 on archives + 8, a recursive mutex, as sub_82644A88 takes it).
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_82644A88);
REX_EXTERN(sub_82626110);
REX_EXTERN(sub_82626170);

namespace {
const std::unordered_map<uint64_t, uint64_t>& lr_jp_text_keys() {
  static const std::unordered_map<uint64_t, uint64_t> keys = [] {
    std::unordered_map<uint64_t, uint64_t> m;
    const char* path = std::getenv("LR_JP_TEXT_MAP");
    if (!path || !path[0]) return m;
    std::ifstream in(path);
    unsigned long long us = 0, jp = 0;
    while (in >> std::hex >> us >> jp) m[us] = jp;
    REXLOG_INFO("LR text: {} Japanese text file(s) listed in {}", m.size(), path);
    return m;
  }();
  return keys;
}
}  // namespace

namespace {
// Lip-sync / motion / voice table files in the Japanese patch (LR_JP_LIPSYNC_LIST, written by
// LR_JP_Voice.ps1): their first lookups are logged with the result, to see the game ask for them.
const std::unordered_map<uint64_t, std::string>& lr_jp_lipsync_keys() {
  static const std::unordered_map<uint64_t, std::string> keys = [] {
    std::unordered_map<uint64_t, std::string> m;
    const char* path = std::getenv("LR_JP_LIPSYNC_LIST");
    if (!path || !path[0]) return m;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      size_t sp = line.find(' ');
      if (sp == std::string::npos) continue;
      m[std::strtoull(line.substr(0, sp).c_str(), nullptr, 16)] = line.substr(sp + 1);
    }
    REXLOG_INFO("LR voice: {} Japanese lip-sync / motion file(s) listed in {}", m.size(), path);
    return m;
  }();
  return keys;
}
}  // namespace

// The lookup with the update archive (the Japanese patch: priority 0) set open or shut just for
// this call, under the archive list's lock; afterwards back to what the voice choice wants.
static void lr_lookup_with_patch(PPCContext& ctx, uint8_t* base, bool open, size_t* changed = nullptr) {
  const uint32_t archives = ctx.r3.u32;
  PPCContext call = ctx;
  call.r3.u64 = archives + 8;
  call.r4.u64 = 0;
  sub_82626110(call, base);
  std::vector<uint32_t> flipped;
  for (uint32_t i = 0; i < 73; ++i) {
    uint32_t entry = archives + i * 288;
    if (!lr_load_be32(base, entry + 32) || (lr_load_be32(base, entry + 52) >> 26) != 0 ||
        (lr_load_be32(base, entry + 48) != 0) == open)
      continue;
    *reinterpret_cast<uint32_t*>(base + entry + 48) = __builtin_bswap32(open ? 1u : 0u);
    flipped.push_back(entry);
  }
  __imp__sub_82644A88(ctx, base);
  if (changed) *changed = flipped.size();
  const uint32_t voice = g_lr_voice_jp.load(std::memory_order_relaxed) ? 1u : 0u;
  for (uint32_t entry : flipped) *reinterpret_cast<uint32_t*>(base + entry + 48) = __builtin_bswap32(voice);
  call.r3.u64 = archives + 8;
  sub_82626170(call, base);
}

// Lip-sync data of a voice line: sub_82374770(loader, key, ...) builds the file code from the
// 16-byte key (sub_82D06990 mode 1, type "Lip" = 10) and loads it through sub_82643A78, which
// ends in the lookup below. Its first loads are logged with the lookups they make, to see
// which lip file the game asks for and whether it is found (Japanese voices: mouths still).
thread_local int t_lr_lip_load = 0;
REX_EXTERN(__imp__sub_82374770);
REX_HOOK_RAW(sub_82374770) {
  static std::atomic<int> logged{0};
  const bool show = ctx.r5.u32 == 0 && logged.fetch_add(1, std::memory_order_relaxed) < 60;
  if (show) {
    std::string hex, text;
    for (uint32_t i = 0; i < 16; ++i) {
      uint8_t c = base[ctx.r4.u32 + i];
      hex += fmt::format("{:02X}", c);
      text += (c >= 32 && c < 127) ? char(c) : '.';
    }
    REXLOG_INFO("LR lip: load key {} \"{}\" ({}, voices {})", hex, text, ctx.r7.u32,
                g_lr_voice_jp.load() ? "Japanese" : "English");
    t_lr_lip_load = 1;
  }
  __imp__sub_82374770(ctx, base);
  if (show) {
    if (t_lr_lip_load == 1) REXLOG_INFO("LR lip:   (no file lookup: already loaded or not found by name)");
    t_lr_lip_load = 0;
  }
}

// Lip-sync of a voice line: sub_82954020 starts it for a character (lip pack key, line name);
// sub_82374B38 finds the data, first in the voice file of the line (an SRD block after its SCD
// data: Japanese voices need theirs), else in the character's lip pack. 0 = no lip-sync.
std::string lr_lip_arg(uint8_t* base, uint64_t v) {
  const uint32_t a = uint32_t(v);
  char t[25];
  if (a >= 0x40000000u && a < 0xA0000000u && lr_guest_cstr(base, a, t, sizeof(t))) {
    bool text = std::strlen(t) >= 3;
    for (const char* c = t; *c && text; ++c) text = *c >= 32 && *c < 127;
    if (text) return fmt::format("{:08X}\"{}\"", a, t);
  }
  return fmt::format("{:X}", v);
}
void lr_lip_trace(const char* name, PPCContext& ctx, uint8_t* base, std::atomic<int>& n, bool after,
                  uint64_t result = 0) {
  if (n.load(std::memory_order_relaxed) >= 80) return;
  if (after) {
    n.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("LR lip:   {} -> {}", name, lr_lip_arg(base, result));
    return;
  }
  REXLOG_INFO("LR lip: {}({}, {}, {}, {}) from {:08X} (voices {})", name, lr_lip_arg(base, ctx.r3.u64),
              lr_lip_arg(base, ctx.r4.u64), lr_lip_arg(base, ctx.r5.u64), lr_lip_arg(base, ctx.r6.u64),
              uint32_t(ctx.lr), g_lr_voice_jp.load() ? "Japanese" : "English");
}
#define LR_LIP_TRACE(fn)                                \
  REX_EXTERN(__imp__##fn);                              \
  REX_HOOK_RAW(fn) {                                    \
    static std::atomic<int> n{0};                       \
    lr_lip_trace(#fn, ctx, base, n, false);             \
    __imp__##fn(ctx, base);                             \
    lr_lip_trace(#fn, ctx, base, n, true, ctx.r3.u64);  \
  }
LR_LIP_TRACE(sub_82954020)
LR_LIP_TRACE(sub_82374B38)

REX_HOOK_RAW(sub_82644A88) {
  if (t_lr_lip_load) {
    PPCContext probe = ctx;
    __imp__sub_82644A88(probe, base);
    REXLOG_INFO("LR lip:   lookup {:X} -> {} (Japanese patch {})", ctx.r4.u64, int32_t(probe.r3.u32),
                g_lr_voice_jp.load() ? "open" : "shut");
    t_lr_lip_load = 2;
  }
  if (const auto& lip = lr_jp_lipsync_keys(); !lip.empty()) {
    auto it = lip.find(ctx.r4.u64);
    static std::atomic<int> shown{0};
    if (it != lip.end() && shown.load(std::memory_order_relaxed) < 40) {
      shown.fetch_add(1, std::memory_order_relaxed);
      PPCContext probe = ctx;
      __imp__sub_82644A88(probe, base);
      REXLOG_INFO("LR voice: game asks for {} (Japanese version in the patch; voices {}; lookup result {})", it->second,
                  g_lr_voice_jp.load() ? "Japanese" : "English", int32_t(probe.r3.u32));
    }
    // LR_JP_MOTION=0 (LR_Play.ps1 -JpMotion off): Japanese voices with the 360's English lip-sync and
    // motion files, to tell whether the voices or the motion files keep the mouths still.
    static const bool english_motion = [] {
      const char* v = std::getenv("LR_JP_MOTION");
      bool on = v && v[0] == '0';
      if (on) REXLOG_INFO("LR voice: Japanese voices with the English lip-sync / motion files (LR_JP_MOTION=0)");
      return on;
    }();
    if (english_motion && it != lip.end() && g_lr_voice_jp.load(std::memory_order_relaxed)) {
      lr_lookup_with_patch(ctx, base, false);
      return;
    }
  }
  // LR_LOOKUP_LOG=1: the first 400 lookups (key) in the log, to check the key format; =all: every one.
  static const int trace = [] {
    const char* v = std::getenv("LR_LOOKUP_LOG");
    return !v ? 0 : v[0] == '1' ? 400 : v[0] == 'a' ? INT32_MAX : 0;
  }();
  if (trace) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1, std::memory_order_relaxed) < trace) {
      uint64_t key = ctx.r4.u64;
      bool jp = g_lr_text_jp.load(std::memory_order_relaxed) && lr_jp_text_keys().count(key);
      REXLOG_INFO("LR lookup: key {:X}{}", key, jp ? " (English text, swapped)" : "");
    }
  }
  if (!g_lr_text_jp.load(std::memory_order_relaxed)) {
    __imp__sub_82644A88(ctx, base);
    return;
  }
  const auto& keys = lr_jp_text_keys();
  auto it = keys.find(ctx.r4.u64);
  if (it == keys.end()) {
    __imp__sub_82644A88(ctx, base);
    return;
  }
  static std::atomic<uint32_t> swapped{0};
  if (swapped.fetch_add(1, std::memory_order_relaxed) < 8)
    REXLOG_INFO("LR text: {:X} -> Japanese {:X}", ctx.r4.u64, it->second);
  ctx.r4.u64 = it->second;
  if (g_lr_voice_jp.load(std::memory_order_relaxed)) {
    __imp__sub_82644A88(ctx, base);
    if (swapped.load(std::memory_order_relaxed) <= 8) REXLOG_INFO("LR text: lookup result {}", int32_t(ctx.r3.u32));
    return;
  }
  size_t opened = 0;
  lr_lookup_with_patch(ctx, base, true, &opened);
  if (swapped.load(std::memory_order_relaxed) <= 8)
    REXLOG_INFO("LR text: lookup result {} (update archive opened for it: {})", int32_t(ctx.r3.u32), opened);
}

// ---------------------------------------------------------------------------------------
// Cheats (Settings > Cheats). The ideas come from two Nexus Mods mods for the Steam release:
// "Free Teleport and Chronostasis" by dragonkir (lightningreturnsfinalfantasy13/mods/13) and the
// "Freeze Timer" cheat table (lightningreturnsfinalfantasy13/mods/22).
//
// Freeze clock: the game clock's update sub_824D01F8(clock) - the 360 build of the Steam function
// the cheat table patches (same constants: 1024, 10010, 75, 86400, 3600, 300000, 24, the cap
// 0xFB75043000) - adds the frame's time to the 64-bit clock at clock+0x50 at its end
// (std r27,0(r25)); the table skips that store. Here the clock is put back to its value from
// before the update, so it stays where it is.
//
// Free Teleport / Chronostasis: the mod sets i17AtbCount of bt_ability's ti810_00 (Teleport) and
// ti840_00 (Chronostasis) to 0 (6000 in the file). The game keeps bt_ability in memory as a WPD
// table: 32-byte entries {u32 record, u32 size 0xC4, 8 bytes, name[16]}; the field is bits 4..20
// of the record's word at +156 (packed low bit first). A host thread finds the two records in the
// guest's committed memory and keeps the field at 0 while the cheat is on, and gives the game's
// own value back when it is turned off.
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_824D01F8);
REX_HOOK_RAW(sub_824D01F8) {
  if (!(g_lr_cheats.load(std::memory_order_relaxed) & 2)) {
    __imp__sub_824D01F8(ctx, base);
    return;
  }
  const uint32_t clock = ctx.r3.u32;
  uint64_t before;
  std::memcpy(&before, base + clock + 0x50, 8);
  __imp__sub_824D01F8(ctx, base);
  uint64_t after;
  std::memcpy(&after, base + clock + 0x50, 8);
  std::memcpy(base + clock + 0x50, &before, 8);
  static std::atomic<bool> logged{false};
  if (before != after && !logged.exchange(true))
    REXLOG_INFO("LR cheats: clock held at {} (the update would have moved it {} ahead)", __builtin_bswap64(before),
                int64_t(__builtin_bswap64(after) - __builtin_bswap64(before)));
}

namespace {
std::atomic<uint8_t*> g_lr_cheat_base{nullptr};

struct LrCheatRecord {
  const char* name;
  const char* label;
  uint32_t entry = 0, record = 0;
  uint32_t saved = 0;  // the game's cost while the cheat holds it at 0 (0: nothing saved)
};

uint32_t lr_cheat_be32(uint8_t* base, uint32_t a) { return lr_load_be32(base, a); }

// memmem (not in the Windows C runtime).
uint8_t* lr_find_bytes(uint8_t* p, size_t n, const char* pat, size_t m) {
  if (!m || n < m) return nullptr;
  for (uint8_t* end = p + n - m + 1; p < end;) {
    p = static_cast<uint8_t*>(std::memchr(p, pat[0], size_t(end - p)));
    if (!p) return nullptr;
    if (std::memcmp(p, pat, m) == 0) return p;
    ++p;
  }
  return nullptr;
}

bool lr_cheat_entry_ok(uint8_t* base, const LrCheatRecord& r) {
  if (!r.entry) return false;
  return lr_cheat_be32(base, r.entry + 4) == 0xC4 && lr_cheat_be32(base, r.entry) == r.record &&
         std::memcmp(base + r.entry + 16, r.name, 9) == 0;
}

// Finds the bt_ability entries of the records in the guest's committed, readable memory.
void lr_cheat_scan(uint8_t* base, LrCheatRecord* recs, size_t n) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  for (uint32_t addr = 0x40000000; addr < 0x80000000;) {
    auto* heap = memory->LookupHeap(addr);
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(addr, &info) || !info.region_size) {
      addr += 0x10000;
      continue;
    }
    uint32_t start = info.base_address, size = info.region_size;
    if ((info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead)) {
      for (size_t k = 0; k < n; ++k) {
        LrCheatRecord& r = recs[k];
        if (lr_cheat_entry_ok(base, r)) continue;
        uint8_t* p = base + start;
        uint8_t* end = p + size;
        while (p + 16 <= end) {
          p = lr_find_bytes(p, size_t(end - p), r.name, 9);  // name with its 0
          if (!p) break;
          uint32_t name_at = uint32_t(p - base);
          if (name_at >= start + 16 && lr_cheat_be32(base, name_at - 12) == 0xC4) {
            r.entry = name_at - 16;
            r.record = lr_cheat_be32(base, r.entry);
            break;
          }
          ++p;
        }
      }
    }
    addr = start + size > addr ? start + size : addr + 0x10000;
  }
}

void lr_cheat_thread() {
  LrCheatRecord recs[2] = {{"ti810_00", "Teleport"}, {"ti840_00", "Chronostasis"}};
  auto last_scan = std::chrono::steady_clock::now() - std::chrono::hours(1);
  bool reported = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    uint8_t* base = g_lr_cheat_base.load();
    if (!base) continue;
    const bool on = g_lr_cheats.load(std::memory_order_relaxed) & 1;
    const bool held = recs[0].saved || recs[1].saved;
    if (!on && !held) continue;
    if ((!lr_cheat_entry_ok(base, recs[0]) || !lr_cheat_entry_ok(base, recs[1])) &&
        std::chrono::steady_clock::now() - last_scan > std::chrono::seconds(10)) {
      last_scan = std::chrono::steady_clock::now();
      lr_cheat_scan(base, recs, 2);
      if (!reported && lr_cheat_entry_ok(base, recs[0])) {
        reported = true;
        REXLOG_INFO("LR cheats: ability table found (Teleport record {:08X}, Chronostasis record {:08X})", recs[0].record,
                    recs[1].record);
      }
    }
    for (auto& r : recs) {
      if (!lr_cheat_entry_ok(base, r)) continue;
      const uint32_t word_at = r.record + 156;
      uint32_t word = lr_cheat_be32(base, word_at);
      const uint32_t cost = (word >> 4) & 0x1FFFF;
      if (on && cost) {
        r.saved = cost;
        word &= ~(0x1FFFFu << 4);
        *reinterpret_cast<uint32_t*>(base + word_at) = __builtin_bswap32(word);
        REXLOG_INFO("LR cheats: {} cost {} -> 0", r.label, cost);
      } else if (!on && r.saved) {
        if (!cost) {
          word |= (r.saved & 0x1FFFF) << 4;
          *reinterpret_cast<uint32_t*>(base + word_at) = __builtin_bswap32(word);
          REXLOG_INFO("LR cheats: {} cost back to {}", r.label, r.saved);
        }
        r.saved = 0;
      }
    }
  }
}

void lr_cheats_frame(uint8_t* base) {
  if (g_lr_cheat_base.exchange(base) == nullptr) std::thread(lr_cheat_thread).detach();
}
}  // namespace

void LrSettingsInit(const std::filesystem::path& dir) {
  g_lr_settings_dir = dir;
  // Voice: LR_VOICE (LR_Play.ps1 -Voice) wins and is saved; then voice.txt; then
  // LR_VOICE_DEFAULT; Japanese when the Japanese voices are installed.
  std::string voice;
  if (const char* env = std::getenv("LR_VOICE"); env && env[0]) {
    voice = env;
    lr_write_setting("voice.txt", voice[0] == 'e' || voice[0] == 'E' ? "en" : "jp");
  } else {
    voice = lr_read_setting("voice.txt");
    if (voice.empty()) {
      const char* fallback = std::getenv("LR_VOICE_DEFAULT");
      voice = fallback && fallback[0] ? fallback : "jp";
    }
  }
  g_lr_voice_jp.store(voice[0] == 'e' || voice[0] == 'E' ? 0 : 1);
  // Text: LR_TEXT (LR_Play.ps1 -Text) wins and is saved; then text.txt; English by default.
  std::string text;
  if (const char* env = std::getenv("LR_TEXT"); env && env[0]) {
    text = env;
    lr_write_setting("text.txt", text[0] == 'j' || text[0] == 'J' ? "jp" : "en");
  } else {
    text = lr_read_setting("text.txt");
  }
  g_lr_text_jp.store(!text.empty() && (text[0] == 'j' || text[0] == 'J') ? 1 : 0);
  // UI style: LR_UI_STYLE overrides ui_style.txt ("auto" - the default -, "360", or "ps3").
  std::string style;
  if (const char* env = std::getenv("LR_UI_STYLE"); env && env[0]) style = env;
  else style = lr_read_setting("ui_style.txt");
  bool automatic = style.empty() || style[0] == 'a' || style[0] == 'A';
  bool ps3 = !style.empty() && (style[0] == 'p' || style[0] == 'P');
  if (automatic && g_lr_pad_ps3.load() >= 0) ps3 = g_lr_pad_ps3.load() != 0;
  g_lr_ui_auto.store(automatic ? 1 : 0);
  lr_set_ui_style(ps3);
  // Frame rate: LR_FPS (LR_Play.ps1 -Fps) wins and is saved; then fps.txt; 30 by default.
  // "60" = everywhere (as before), "60g" = gameplay only.
  std::string fps;
  if (const char* env = std::getenv("LR_FPS"); env && env[0]) {
    fps = env;
    lr_write_setting("fps.txt", fps == "60" || fps == "60g" ? fps.c_str() : "30");
  } else {
    fps = lr_read_setting("fps.txt");
  }
  g_lr_fps60.store(fps == "60" ? 2 : fps == "60g" ? 1 : 0);
  g_lr_confirm_b.store(lr_read_setting("confirm.txt") == "b" ? 1 : 0);
  // Anti-aliasing: LR_AA (LR_Play.ps1 -AA) wins and is saved; then aa.txt; then LR_AA_DEFAULT; off.
  std::string aa;
  if (const char* env = std::getenv("LR_AA"); env && env[0]) {
    aa = env;
    lr_write_setting("aa.txt", aa.c_str());
  } else {
    aa = lr_read_setting("aa.txt");
    if (aa.empty())
      if (const char* d = std::getenv("LR_AA_DEFAULT"); d && d[0]) aa = d;
  }
  g_lr_aa.store(aa == "extreme" ? 2 : aa == "fxaa" ? 1 : 0);
  // Cheats: LR_CHEATS (0-3) wins and is saved; then cheats.txt; off.
  std::string cheats;
  if (const char* env = std::getenv("LR_CHEATS"); env && env[0]) {
    cheats = env;
    lr_write_setting("cheats.txt", cheats.c_str());
  } else {
    cheats = lr_read_setting("cheats.txt");
  }
  g_lr_cheats.store(cheats.size() == 1 && cheats[0] >= '0' && cheats[0] <= '3' ? cheats[0] - '0' : 0);
  REXLOG_INFO("LR settings: voice {}, text {}, UI style {}{}, frame rate {} ({})", g_lr_voice_jp.load() ? "Japanese" : "English",
              g_lr_text_jp.load() ? "Japanese" : "English",
              ps3 ? "PS3" : "360", automatic ? " (auto: follows the controller)" : "",
              g_lr_fps60.load() == 2 ? "60" : g_lr_fps60.load() ? "60 (cutscenes 30)" : "30", dir.string());
}

// ---------------------------------------------------------------------------------------
// Confirm button (the PC version's Control_ConfirmButtonLayout). The game keeps which pad
// button confirms and which cancels in two masks, plus a flag that orders the face-button
// icons (sub_82327D30), all set once at start in sub_822F44B0: the 360 release fixes them at
// confirm 0x20 (A), cancel 0x80 (B), flag 1. The PC's other layout swaps the masks and
// clears the flag (B confirms, A cancels: the Japanese layout). Every menu reads the masks,
// so writing them switches the layout at once.
// ---------------------------------------------------------------------------------------
constexpr uint32_t kConfirmMask = 0x830EDD00, kCancelMask = 0x830EDD04, kConfirmIconFlag = 0x830EDD08;
std::atomic<uint8_t*> g_lr_confirm_base{nullptr};

void lr_apply_confirm(uint8_t* base) {
  if (!base) return;
  auto st = [&](uint32_t a, uint32_t v) {
    base[a] = uint8_t(v >> 24), base[a + 1] = uint8_t(v >> 16), base[a + 2] = uint8_t(v >> 8), base[a + 3] = uint8_t(v);
  };
  const bool b = g_lr_confirm_b.load() != 0;
  st(kConfirmMask, b ? 0x80 : 0x20);
  st(kCancelMask, b ? 0x20 : 0x80);
  base[kConfirmIconFlag] = b ? 0 : 1;
}

REX_EXTERN(__imp__sub_822F44B0);
REX_HOOK_RAW(sub_822F44B0) {
  __imp__sub_822F44B0(ctx, base);
  g_lr_confirm_base.store(base);
  if (g_lr_confirm_b.load()) {
    lr_apply_confirm(base);
    REXLOG_INFO("LR confirm button: B confirms, A cancels");
  }
}

int LrConfigGetConfirm() { return g_lr_confirm_b.load(); }
void LrConfigSetConfirm(int b) {
  g_lr_confirm_b.store(b ? 1 : 0);
  lr_write_setting("confirm.txt", b ? "b" : "a");
  lr_apply_confirm(g_lr_confirm_base.load());
  REXLOG_INFO("LR confirm button: {} chosen", b ? "B (A cancels)" : "A (B cancels)");
}

int LrConfigGetAa() { return g_lr_aa.load(); }
void LrConfigSetAa(int choice) {
  choice = choice < 0 || choice > 2 ? 0 : choice;
  g_lr_aa.store(choice);
  lr_write_setting("aa.txt", choice == 2 ? "extreme" : choice == 1 ? "fxaa" : "off");
  REXLOG_INFO("LR anti-aliasing: {} chosen", choice == 2 ? "FXAA extreme" : choice == 1 ? "FXAA" : "off");
}

// Settings > Cheats: 0 off, 1 free Teleport / Chronostasis, 2 freeze the clock, 3 both.
int LrConfigGetCheats() { return g_lr_cheats.load(); }
void LrConfigSetCheats(int choice) {
  choice = choice < 0 || choice > 3 ? 0 : choice;
  g_lr_cheats.store(choice);
  lr_write_setting("cheats.txt", std::to_string(choice).c_str());
  REXLOG_INFO("LR cheats: free Teleport / Chronostasis {}, clock {}", choice & 1 ? "on" : "off",
              choice & 2 ? "frozen" : "running");
}

int LrConfigGetFps60() { return g_lr_fps60.load(); }
void LrConfigSetFps60(int choice) {
  choice = choice < 0 || choice > 2 ? 0 : choice;
  g_lr_fps60.store(choice);
  lr_write_setting("fps.txt", choice == 2 ? "60" : choice == 1 ? "60g" : "30");
  REXLOG_INFO("LR frame rate: {} chosen", choice == 2 ? "60 fps" : choice == 1 ? "60 fps, cutscenes 30" : "30 fps");
}

// ---------------------------------------------------------------------------------------
// Frame rate. The frame pacer (sub_82ABD790 builds it, [0x83147A50]) keeps the display's
// refresh rate at +0 and the target frame rate at +4. Each frame the main loop
// (sub_82AB3AA0) sets the target from the current scene (sub_82ABCF38(pacer, fps): 29.97
// in the field and in battle, 59.94 on the title screen), and sub_82ABCF78 turns it into
// "refreshes per frame" = round(refresh / target) for the device (sub_82A81B48 ->
// device+6964): the main thread then waits for that many vertical blanks (sub_82ABD218).
// The game's frame time is not the target but the measured time between the last two
// vertical-blank timestamps of the pacer, rounded to whole refreshes (sub_82ABD218: the
// timestamps are microseconds, the result in 1/300000 s), so game speed does not depend on
// the frame rate. With 60 fps on, scenes that ask for 30 get 59.94 instead.
//
// What the game asks for comes from a frame-rate mode stack (sub_822F0FC0(app, push, mode),
// current mode at app+788): 0 = 59.94 (title, system screens), 1 = 29.97 (field, battle,
// menus), 2+ = 29.97 (event scenes, animated for 30). The PC version's "Dynamic" frame rate
// runs mode 1 at 59.94 and keeps the scenes at 29.97; "60 fps (gameplay)" does the same.
// ---------------------------------------------------------------------------------------
std::atomic<uint32_t> g_lr_fps_app{0};  // the object whose +788 is the frame-rate mode

REX_EXTERN(__imp__sub_822F0FC0);
REX_HOOK_RAW(sub_822F0FC0) {
  g_lr_fps_app.store(ctx.r3.u32, std::memory_order_relaxed);
  __imp__sub_822F0FC0(ctx, base);
}

// The frame rate the game runs at (after the 60 fps setting); the Android performance hint uses it
// as the frame target.
static std::atomic<double> g_lr_frame_rate_asked{30000.0 / 1001.0};

REX_EXTERN(__imp__sub_82ABCF38);
REX_HOOK_RAW(sub_82ABCF38) {
  double asked = ctx.f1.f64;
  const int setting = g_lr_fps60.load(std::memory_order_relaxed);
  int mode = -1;
  if (uint32_t app = g_lr_fps_app.load(std::memory_order_relaxed)) {
    const uint8_t* m = base + app + 788;
    mode = int(uint32_t(m[0]) << 24 | uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3]);
  }
  const bool scene = mode >= 2;  // an event scene: stays at 30 with "60 fps (gameplay)"
  bool raise = setting && asked > 20.0 && asked < 50.0 && (setting == 2 || !scene);
  if (raise) ctx.f1.f64 = 60000.0 / 1001.0;
  g_lr_frame_rate_asked.store(ctx.f1.f64, std::memory_order_relaxed);
  static std::atomic<int> last{-1};
  int now = (raise ? 1000 : int(asked + 0.5)) + 10000 * (mode + 1);
  if (last.exchange(now) != now)
    REXLOG_INFO("LR frame rate: the game asks for {:.2f} fps (mode {}){}", asked, mode,
                raise ? ", running at 59.94 (60 fps setting)" : "");
  __imp__sub_82ABCF38(ctx, base);
  void LrKeyIconsFrame(uint8_t* base);  // lr_keyboard_icons.inc
  LrKeyIconsFrame(base);
}

int LrConfigGetVoice() { return g_lr_voice_jp.load(); }
// 1 when the Japanese voices are installed (the update archive is registered), 0 when not,
// -1 before the game has registered its archives.
int LrConfigVoiceAvailable() { return g_lr_voice_archives.load() < 0 ? -1 : g_lr_voice_archives.load() > 0; }
void LrConfigSetVoice(int jp) {
  g_lr_voice_jp.store(jp ? 1 : 0);
  lr_write_setting("voice.txt", jp ? "jp" : "en");
}
int LrConfigGetText() { return g_lr_text_jp.load(); }
namespace {
const std::unordered_map<uint64_t, uint64_t>& lr_jp_text_keys();
}
int LrConfigTextAvailable() { return lr_jp_text_keys().empty() ? 0 : 1; }
void LrConfigSetText(int jp) {
  g_lr_text_jp.store(jp ? 1 : 0);
  lr_write_setting("text.txt", jp ? "jp" : "en");
  REXLOG_INFO("LR text: {} (fully at the next start)", jp ? "Japanese" : "English");
}

int LrConfigGetUiStyle() { return g_lr_ui_ps3.load(); }
// A fixed choice (Config menu, F9 / Back + R3): no longer follows the controller.
void LrConfigSetUiStyle(int ps3) {
  g_lr_ui_auto.store(0);
  lr_set_ui_style(ps3);
  lr_write_setting("ui_style.txt", ps3 ? "ps3" : "360");
  REXLOG_INFO("UI style: {}", ps3 ? "PS3" : "360");
}
// Config menu "Button Prompts": 0 Auto, 1 Xbox 360, 2 PlayStation 3.
int LrConfigGetUiMode() { return g_lr_ui_auto.load() ? 0 : g_lr_ui_ps3.load() ? 2 : 1; }
void LrConfigSetUiMode(int mode) {
  if (mode == 1 || mode == 2) {
    LrConfigSetUiStyle(mode == 2);
    return;
  }
  g_lr_ui_auto.store(1);
  lr_write_setting("ui_style.txt", "auto");
  if (g_lr_pad_ps3.load() >= 0) lr_set_ui_style(g_lr_pad_ps3.load());
  REXLOG_INFO("UI style: auto ({} for the controller in use)", g_lr_ui_ps3.load() ? "PS3" : "360");
}
// The controller in use changed (ps3: DualShock / DualSense; else an Xbox controller).
void LrUiStyleForController(int ps3, const char* name) {
  g_lr_pad_ps3.store(ps3 ? 1 : 0);
  if (!g_lr_ui_auto.load() || (ps3 ? 1 : 0) == g_lr_ui_ps3.load()) return;
  lr_set_ui_style(ps3);
  REXLOG_INFO("LR input: {} -> Button Prompts {} (auto)", name ? name : "controller", ps3 ? "PS3" : "360");
}

// ---------------------------------------------------------------------------------------
// Render resolution: the Config menu's "Resolution" item and LR_Play.ps1 -Resolution.
// The renderer reads its draw resolution scale (cvar resolution_scale: 1 = 1280x720,
// 2 = 2560x1440, 3 = 3840x2160) once, when the plugin starts, and sizes its render targets
// and texture cache by it; a change therefore applies at the next start. LrResolutionStartup
// runs before the plugin is loaded (OnPreSetup) and hands the choice over as
// REX_RESOLUTION_SCALE: the plugin registers its cvars after lightningreturns.toml was read,
// and a late registration takes the environment over the config file. The game's own restart
// on returning to the title screen (lr_relaunch) runs it again, so a menu choice applies there.
// Choice: LR_RESOLUTION (LR_Play.ps1 -Resolution: saved, then removed so that the restart uses
// the saved choice), else Documents\lightningreturns\resolution.txt: auto, 720, 1440, or 2160.
// Auto = the tallest display's height / 720 rounded up, at most 3 (a 1080p display renders at
// 1440p, scaled down), as LR_Play.ps1 computes it.
// ---------------------------------------------------------------------------------------
#if defined(_WIN32)
#pragma comment(lib, "user32.lib")
#endif
#include <rex/cvar.h>
#include <rex/platform/env.h>

namespace {
constexpr const char* kLrResolutionNames[] = {"auto", "720", "1440", "2160"};
std::atomic<int> g_lr_res_choice{0};        // 0 auto, 1 720p, 2 1440p, 3 2160p
std::atomic<int> g_lr_res_active_scale{0};  // scale this process renders at, 0 = not known

int lr_res_parse(std::string v) {
  while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
  if (v == "720" || v == "720p" || v == "1") return 1;
  if (v == "1440" || v == "1440p" || v == "2") return 2;
  if (v == "2160" || v == "2160p" || v == "4k" || v == "4K" || v == "3") return 3;
  return 0;
}

// Height of the tallest display attached to the desktop, 0 when not known.
uint32_t lr_display_height() {
#if defined(_WIN32)
  uint32_t tallest = 0;
  DISPLAY_DEVICEW device = {};
  device.cb = sizeof(device);
  for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
    if (device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
      DEVMODEW mode = {};
      mode.dmSize = sizeof(mode);
      if (EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
        tallest = std::max(tallest, uint32_t(mode.dmPelsHeight));
    }
    device = {};
    device.cb = sizeof(device);
  }
  return tallest;
#else
  return 0;
#endif
}
}  // namespace

// Scale a choice renders at; for Auto 0 when the display is not known (the config file's
// resolution_scale stays then).
int LrResolutionScaleFor(int choice) {
#if defined(__ANDROID__)
  return choice >= 2 ? 2 : 1;  // Android: 720p (default, also for Auto) or 1440p only
#endif
  if (choice >= 1 && choice <= 3) return choice;
  uint32_t height = lr_display_height();
  if (!height) return 0;
  return int(std::min<uint32_t>(3, std::max<uint32_t>(1, (height + 719) / 720)));
}

// Before the renderer plugin is loaded.
void LrResolutionStartup(const std::filesystem::path& settings_dir) {
  if (g_lr_settings_dir.empty()) g_lr_settings_dir = settings_dir;
  int choice;
  auto requested = rex::platform::env::get("LR_RESOLUTION");
  if (requested && !requested->empty()) {
    choice = lr_res_parse(*requested);
    lr_write_setting("resolution.txt", kLrResolutionNames[choice]);
    rex::platform::env::unset("LR_RESOLUTION");
  } else {
    choice = lr_res_parse(lr_read_setting("resolution.txt"));
  }
  // Safe mode (the PC version's Validation.ini): a resolution is on trial until the title menu
  // appears (LrResolutionReachedTitle). If the last start with it never got there - the GPU
  // could not take it, say - this start goes back to the last one that did.
  {
    const std::string good = lr_read_setting("resolution_ok.txt");
    const std::string trial = lr_read_setting("resolution_trial.txt");
    const int good_choice = good.empty() ? 0 : lr_res_parse(good);
    if (choice != good_choice && trial == kLrResolutionNames[choice]) {
      REXLOG_WARN("LR resolution: the last start with {} did not reach the title screen; back to {}",
                  kLrResolutionNames[choice], kLrResolutionNames[good_choice]);
      choice = good_choice;
      lr_write_setting("resolution.txt", kLrResolutionNames[choice]);
      lr_write_setting("resolution_trial.txt", "");
    } else if (choice != good_choice) {
      lr_write_setting("resolution_trial.txt", kLrResolutionNames[choice]);
    }
  }
  g_lr_res_choice.store(choice);
  int scale = LrResolutionScaleFor(choice);
  if (scale) {
    rex::platform::env::set("REX_RESOLUTION_SCALE", std::to_string(scale));
    g_lr_res_active_scale.store(scale);
  }
  REXLOG_INFO("LR resolution: {}{} ({})", kLrResolutionNames[choice],
              scale ? fmt::format(" -> {}x{}", 1280 * scale, 720 * scale) : std::string(" -> config file"),
              (settings_dir / "resolution.txt").string());
#if defined(__ANDROID__)
  if (auto options = rex::platform::env::get("LR_ANDROID_OPTIONS_LOG")) {
    std::string_view rest = *options;
    while (!rest.empty()) {
      size_t end = rest.find('\n');
      std::string_view line = rest.substr(0, end);
      if (!line.empty()) REXLOG_INFO("{}", line);
      if (end == std::string_view::npos) break;
      rest.remove_prefix(end + 1);
    }
  }
#endif
}

int LrConfigGetResolution() { return g_lr_res_choice.load(); }

// The title menu is up: this start's resolution works (see the safe mode above).
void LrResolutionReachedTitle() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  const char* name = kLrResolutionNames[g_lr_res_choice.load()];
  if (lr_read_setting("resolution_ok.txt") != name) lr_write_setting("resolution_ok.txt", name);
  if (!lr_read_setting("resolution_trial.txt").empty()) lr_write_setting("resolution_trial.txt", "");
}
// The renderer's scale is fixed while it runs (a live rebuild through the plugin's
// lr_set_resolution_scale export lost the menus and fonts the game had built on the GPU), so a
// new size applies after a restart. lr_plugin_set_resolution_scale is kept for diagnostics.
int LrResolutionActiveScale();

using LrSetResolutionScaleFn = void (*)(int32_t);
LrSetResolutionScaleFn lr_plugin_set_resolution_scale() {
  static const LrSetResolutionScaleFn fn = []() -> LrSetResolutionScaleFn {
#if defined(_WIN32)
    HMODULE plugin = GetModuleHandleW(L"rexgpu-xenos.dll");
    return plugin ? reinterpret_cast<LrSetResolutionScaleFn>(GetProcAddress(plugin, "lr_set_resolution_scale"))
                  : nullptr;
#else
    std::string path;
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* out) {
          if (info->dlpi_name && std::strstr(info->dlpi_name, "rexgpu-xenos")) {
            *static_cast<std::string*>(out) = info->dlpi_name;
            return 1;
          }
          return 0;
        },
        &path);
    void* plugin = path.empty() ? nullptr : dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    return plugin ? reinterpret_cast<LrSetResolutionScaleFn>(dlsym(plugin, "lr_set_resolution_scale")) : nullptr;
#endif
  }();
  return fn;
}

// Stores the choice. A different render size needs a restart: the Settings menu then asks
// in the game's own Yes/No dialog (lr_config_menu.inc) and calls LrResolutionRestartNow on Yes.
bool LrConfigSetResolution(int choice) {
  choice = std::clamp(choice, 0, 3);
  g_lr_res_choice.store(choice);
  lr_write_setting("resolution.txt", kLrResolutionNames[choice]);
  int scale = LrResolutionScaleFor(choice);
  bool restart = scale && scale != LrResolutionActiveScale();
  REXLOG_INFO("LR resolution: {} chosen ({}{})", kLrResolutionNames[choice],
              scale ? fmt::format("{}x{}", 1280 * scale, 720 * scale) : std::string("config file"),
              restart ? "; needs a restart" : "; already in use");
  return restart;
}

void LrResolutionRestartNow() {
  if (g_lr_relaunching.exchange(true)) return;
  REXLOG_INFO("LR resolution: restarting the game now to apply the new size");
  ::rex::FlushLogging();
  if (lr_restart_process()) lr_end_process();
  REXLOG_WARN("LR resolution: could not restart; the choice applies at the next start");
  g_lr_relaunching.store(false);
}

// The in-game main menu's Settings (sub_827A13B8): the game has left the title.
REX_EXTERN(__imp__sub_827A13B8);
REX_HOOK_RAW(sub_827A13B8) {
  lr_leave_title("in-game Settings opened", true);
  __imp__sub_827A13B8(ctx, base);
}

// Releasing an object's 16 resource slots (sub_82911BD8; 304-byte slots from +160, each starting
// with 8 handles into a 632-entry table, -1 = none). A handle outside the table makes the game's
// lookup return null and crash on it (seen on the user's PC while loading a save, handle
// 01340000; the same 632-entry lookup crashed mixed Steam/360 saves on PC). Such a handle can
// never be released, so it is cleared to "none" first.
REX_EXTERN(__imp__sub_82911BD8);
REX_HOOK_RAW(sub_82911BD8) {
  static std::atomic<int> logged{0};
  uint32_t slots = ctx.r3.u32 + 160;
  for (uint32_t i = 0; i < 16; ++i) {
    for (uint32_t j = 0; j < 8; ++j) {
      uint32_t address = slots + i * 304 + j * 4;
      uint32_t handle = lr_load_be32(base, address);
      if (handle == 0xFFFFFFFFu || handle < 632) continue;
      if (logged.fetch_add(1) < 8)
        REXLOG_WARN("LR resources: invalid handle {:08X} in slot {} of object {:08X}; cleared", handle, i,
                    ctx.r3.u32);
      *reinterpret_cast<uint32_t*>(base + address) = 0xFFFFFFFFu;
    }
  }
  __imp__sub_82911BD8(ctx, base);
}

// Scale this process renders at: what the renderer reports, else what startup chose, else the
// renderer's own setting.
int LrResolutionActiveScale() {
  using GetFn = int32_t (*)();
  static const GetFn get = []() -> GetFn {
#if defined(_WIN32)
    HMODULE plugin = GetModuleHandleW(L"rexgpu-xenos.dll");
    return plugin ? reinterpret_cast<GetFn>(GetProcAddress(plugin, "lr_get_resolution_scale")) : nullptr;
#else
    std::string path;
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* out) {
          if (info->dlpi_name && std::strstr(info->dlpi_name, "rexgpu-xenos")) {
            *static_cast<std::string*>(out) = info->dlpi_name;
            return 1;
          }
          return 0;
        },
        &path);
    void* plugin = path.empty() ? nullptr : dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    return plugin ? reinterpret_cast<GetFn>(dlsym(plugin, "lr_get_resolution_scale")) : nullptr;
#endif
  }();
  if (get) {
    if (int32_t scale = get(); scale > 0) return scale;
  }
  if (int scale = g_lr_res_active_scale.load()) return scale;
  int scale = std::atoi(rex::cvar::GetFlagByName("resolution_scale").c_str());
  return std::clamp(scale, 1, 3);
}

// ---------------------------------------------------------------------------------------
// Outerworld (the game's "SNS" feature) terms when Settings > Outerworld Services is switched
// to connect (sub_8278F7F0, item 10, builds this step with sub_8278C748). sub_8278AD00 is its
// step function (state at +164):
//   1: wait for the request at +144 to finish (its +12 = 2 or 3), then open the Terms of Use
//      document (sub_827EF868(widget, 1))
//   2: document closed -> release +144; if the flag sub_827EC3B8 is 0 (not agreed) finish,
//      else 3
//   3: $sys_sns002 "Do you agree to the Terms of Use ...? |I agree|I disagree" -> finish
// Finishing means +156 = 1 (done), +160 = result, +164 = 0. The hook takes the "not agreed"
// exit as soon as state 1 would open the document, then lets the original run with state 0,
// which only does the common tail every state ends with. LR_OUTERWORLD_PROMPT=1 keeps it.
// (sub_8278AF68, the twin with the same document class and yes_no = 0, is the Open-Source
// Software Licenses viewer and is left alone.)
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8278AD00);
REX_EXTERN(__imp__sub_8270CEF0);
REX_HOOK_RAW(sub_8278AD00) {
  uint32_t self = ctx.r3.u32;
  auto store = [&](uint32_t offset, uint32_t value) {
    *reinterpret_cast<uint32_t*>(base + self + offset) = __builtin_bswap32(value);
  };
  if (!lr_keep_outerworld_prompt() && lr_load_be32(base, self + 164) == 1) {
    uint32_t request = lr_load_be32(base, self + 144);
    uint32_t request_state = request ? lr_load_be32(base, request + 12) : 0;
    if (!request || request_state == 2 || request_state == 3) {
      if (request) {
        auto f1 = ctx.f1;  // the frame time the common tail passes on
        ctx.r3.u64 = lr_load_be32(base, 0x831203AC);
        ctx.r4.u64 = request;
        ctx.lr = 0x8278ADF4;
        __imp__sub_8270CEF0(ctx, base);
        ctx.f1 = f1;
      }
      store(144, 0);
      store(148, 0);
      store(156, 1);
      store(160, 0);
      store(164, 0);
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_INFO("LR Outerworld: terms of use skipped (Settings)");
      }
      // With state 0 the original only runs its common tail (sub_827DDC78, the base update).
      ctx.r3.u64 = self;
    }
  }
  __imp__sub_8278AD00(ctx, base);
}

// ---------------------------------------------------------------------------------------
// Hang reports. sub_829CB8F0 is the game's VdSwap call (once per frame). If frames stop
// for 6 s after the game has started drawing, the stacks of every thread are written to
// LR_HANG_DIR (LR_Play.ps1 sets it to the play folder) as three minidumps 1 s apart.
// ---------------------------------------------------------------------------------------
namespace {
std::atomic<uint64_t> g_lr_frames{0};
void lr_log_current_waits();
void lr_log_movie_state();

#if defined(_WIN32)
struct LrHangWatch {
  LrHangWatch() {
    std::thread([] {
      uint64_t last = 0;
      auto last_change = std::chrono::steady_clock::now();
      int reports = 0;
      bool reported = false;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        uint64_t now_frames = g_lr_frames.load();
        auto now = std::chrono::steady_clock::now();
        if (now_frames != last) {
          last = now_frames;
          last_change = now;
          reported = false;
          continue;
        }
        if (!now_frames || reported || reports >= 2 || now - last_change < std::chrono::seconds(6)) continue;
        reported = true;
        ++reports;
        std::filesystem::path dir = lr_env_path("LR_HANG_DIR");
        if (dir.empty()) dir = std::filesystem::temp_directory_path();
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
        auto write = dbghelp ? reinterpret_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
        REXLOG_WARN("LR hang: no frame for 6 s (after {} frames); writing thread stacks to {}", now_frames,
                    dir.string());
        lr_log_current_waits();
        lr_log_movie_state();
        ::rex::FlushLogging();
        for (int i = 0; write && i < 3; ++i) {
          wchar_t name[64];
          swprintf(name, 64, L"hang%d_%d.dmp", reports, i);
          HANDLE f = CreateFileW((dir / name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
          if (f != INVALID_HANDLE_VALUE) {
            // MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules: stacks of every thread.
            write(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1000 | 0x20, nullptr, nullptr, nullptr);
            CloseHandle(f);
          }
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        REXLOG_WARN("LR hang: reports written");
        ::rex::FlushLogging();
      }
    }).detach();
  }
} g_lr_hang_watch;
#endif
}  // namespace

// ---------------------------------------------------------------------------------------
// Slow-frame breakdown. Every guest wait / sleep wrapper below is timed on the thread that
// calls it; the thread that presents frames (sub_829CB8F0) is the game's main thread, and for
// each of its frames over 45 ms the log gets one "LR slow frame" line splitting the frame into
// game code, sleeping, waiting on other threads (with the slowest wait and its caller),
// waiting for the renderer, and presenting. A summary for the main thread comes every 10 s.
// ---------------------------------------------------------------------------------------
namespace {
enum LrWaitKind { kLrSleep, kLrWait, kLrRenderer, kLrKinds };
struct LrWaitAcc {
  uint64_t us[kLrKinds] = {};
  uint32_t count[kLrKinds] = {};
  uint64_t longest_us = 0;
  uint32_t longest_fn = 0, longest_lr = 0;
  int depth = 0;
  void reset() { *this = LrWaitAcc(); }
};
thread_local LrWaitAcc lr_wait_acc;

// Per-thread wait totals by call site (return address into the game), for the 10 s report.
struct LrThreadWaits {
  int index = 0;
  std::atomic<uint32_t> current_site{0};  // caller of the wait this thread is in now (0: none)
  std::atomic<uint32_t> current_fn{0};
  std::atomic<uint64_t> current_since_us{0};
  std::atomic<uint32_t> current_sp{0};          // guest r1 at the start of that wait
  std::atomic<const PPCContext*> ctx{nullptr};  // this thread's guest registers
  std::atomic<bool> dead{false};                // the thread has exited
  std::atomic<uint32_t> job{0};                 // job function it runs now (job system), 0: none
  std::atomic<uint64_t> fault_us{0}, faults{0};  // time in page faults (Windows)
  int tid = 0;                                   // OS thread id
#if defined(_WIN32)
  HANDLE handle = nullptr;  // for its CPU time (QueryThreadCycleTime)
#else
  clockid_t cpu_clock{};
  bool has_cpu_clock = false;
#endif
  uint64_t last_cpu_us = 0;  // CPU time at the last 10 s report
  std::mutex mutex;
  std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> sites;  // caller -> (us, count)
};
std::mutex lr_thread_waits_mutex;
std::vector<std::shared_ptr<LrThreadWaits>> lr_thread_waits;

// CPU time a thread has used, in microseconds (0 if unknown). Windows counts cycles; they are
// converted with the TSC rate measured against the steady clock.
#if defined(_WIN32)
double lr_tsc_per_us() {
  static const double rate = [] {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t c0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint64_t c1 = __rdtsc();
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    return us > 0 ? double(c1 - c0) / us : 3000.0;
  }();
  return rate;
}
#endif
uint64_t lr_thread_cpu_us(LrThreadWaits& w) {
#if defined(_WIN32)
  ULONG64 cycles = 0;
  if (!w.handle || !QueryThreadCycleTime(w.handle, &cycles)) return 0;
  return uint64_t(double(cycles) / lr_tsc_per_us());
#else
  timespec ts{};
  if (!w.has_cpu_clock || clock_gettime(w.cpu_clock, &ts) != 0) return 0;
  return uint64_t(ts.tv_sec) * 1000000 + uint64_t(ts.tv_nsec) / 1000;
#endif
}

struct LrThreadWaitsHolder {
  std::shared_ptr<LrThreadWaits> w;
  ~LrThreadWaitsHolder() {
    if (!w) return;
    w->ctx.store(nullptr);
    w->dead.store(true);
#if defined(_WIN32)
    if (w->handle) CloseHandle(w->handle);
    w->handle = nullptr;
#endif
  }
};
LrThreadWaits& lr_my_waits() {
  thread_local LrThreadWaitsHolder mine;
  if (!mine.w) {
    mine.w = std::make_shared<LrThreadWaits>();
#if defined(_WIN32)
    mine.w->handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
    mine.w->tid = int(GetCurrentThreadId());
#else
    mine.w->tid = int(gettid());
    mine.w->has_cpu_clock = pthread_getcpuclockid(pthread_self(), &mine.w->cpu_clock) == 0;
#endif
    std::lock_guard<std::mutex> lock(lr_thread_waits_mutex);
    mine.w->index = int(lr_thread_waits.size());
    lr_thread_waits.push_back(mine.w);
  }
  return *mine.w;
}

void lr_wait_note(LrWaitKind kind, uint32_t fn, uint32_t lr, uint64_t us) {
  if (kind != kLrRenderer) {
    auto& w = lr_my_waits();
    std::lock_guard<std::mutex> lock(w.mutex);
    auto& site = w.sites[lr];
    site.first += us;
    site.second++;
  }
  auto& a = lr_wait_acc;
  a.us[kind] += us;
  a.count[kind]++;
  if (kind != kLrRenderer && us > a.longest_us) {
    a.longest_us = us;
    a.longest_fn = fn;
    a.longest_lr = lr;
  }
}

// Logged when frames stop: what every game thread is blocked in right now.
// Return addresses on a guest stack: every frame starts with the back chain (the caller's
// r1) and functions save LR at -8 from their caller's r1 before stwu, so each step up is
// one call site. Read from another thread while the game runs, hence the checks.
std::atomic<uint8_t*> g_lr_guest_base{nullptr};

// Reads a guest word without faulting: a stack pointer read from another thread can point at
// memory that is not mapped (a stack being torn down, a half-written frame), and the runtime
// does not recover from a host read of unmapped guest memory (it crashes or retries forever).
// The first read in each 4 KB page goes through the kernel, which fails instead of faulting.
struct LrSafeGuestReader {
  uint8_t* base;
  uint32_t checked_page = ~0u;
  bool Read32(uint32_t address, uint32_t& out) {
    if (address & 3) return false;
    if ((address >> 12) != checked_page) {
      uint32_t value = 0;
#if defined(_WIN32)
      SIZE_T read = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), base + address, &value, 4, &read) || read != 4) {
        return false;
      }
#else
      iovec local{&value, 4}, remote{base + address, 4};
      if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != 4) return false;
#endif
      checked_page = address >> 12;
      out = __builtin_bswap32(value);
      return true;
    }
    out = lr_load_be32(base, address);
    return true;
  }
};

std::string lr_guest_stack(uint32_t sp, int max_depth = 20) {
  uint8_t* base = g_lr_guest_base.load(std::memory_order_relaxed);
  std::string out;
  LrSafeGuestReader reader{base};
  for (int depth = 0; base && depth < max_depth; ++depth) {
    if (sp < 0x10000000 || sp >= 0xE0000000 || (sp & 3)) break;
    uint32_t caller_sp, ret;
    if (!reader.Read32(sp, caller_sp)) break;
    if (caller_sp <= sp || caller_sp - sp > (1u << 20)) break;
    if (!reader.Read32(caller_sp - 8, ret)) break;
    if (ret < 0x82000000 || ret >= 0x83400000) break;
    out += fmt::format(" {:08X}", ret);
    sp = caller_sp;
  }
  return out.empty() ? " (unreadable)" : out;
}

void lr_log_current_waits() {
  std::vector<std::shared_ptr<LrThreadWaits>> threads;
  {
    std::lock_guard<std::mutex> lock(lr_thread_waits_mutex);
    threads = lr_thread_waits;
  }
  uint64_t now = lr_now_us();
  for (auto& w : threads) {
    uint32_t site = w->current_site.load(std::memory_order_acquire);
    if (site) {
      uint64_t since = w->current_since_us.load();
      REXLOG_WARN("LR hang: game thread {} waiting {} ms in sub_{:08X} called from {:08X}; calls:{}", w->index,
                  (now > since ? now - since : 0) / 1000, w->current_fn.load(), site,
                  lr_guest_stack(w->current_sp.load()));
    } else {
      const PPCContext* c = w->ctx.load(std::memory_order_relaxed);
      REXLOG_WARN("LR hang: game thread {} not in a guest wait (running, spinning, or in a lock); lr {:08X}; calls:{}",
                  w->index, c ? uint32_t(c->lr) : 0u, c ? lr_guest_stack(c->r1.u32) : std::string(" ?"));
    }
  }
}

void lr_log_movie_state() {
  auto& t = g_lr_movie_trace;
  const char* name = t.name.load();
  if (!name) {
    REXLOG_WARN("LR hang: no PS3 movie was played");
    return;
  }
  uint64_t now = lr_now_us(), call = t.last_call_us.load(), write = t.last_write_us.load();
  REXLOG_WARN("LR hang: movie {} {}; last frame asked {} ({} ms ago), {} shown (last {} ms ago), {} not ready, "
              "{} restarts, game thread {} the decoder, writer {}",
              name, t.open.load() ? "open" : "closed", t.frame.load(), call ? (now - call) / 1000 : 0,
              t.shown.load(), write ? (now - write) / 1000 : 0, t.late.load(), t.restarts.load(),
              t.in_get.load() ? "inside" : "not in", t.in_write.load() ? "copying" : "idle");
}

struct LrFrameTotals {
  uint64_t frames = 0, slow = 0, frame_us = 0, us[kLrKinds] = {}, present_us = 0, faults = 0, fault_us = 0;
} lr_main_totals;

// Page faults: a CPU write to memory the renderer watches raises an access violation that
// the runtime handles (invalidating the GPU copy). Counted per thread to see whether the
// main thread's "game code" time is really fault handling. Registered on the first frame,
// after the runtime's own handler, so this one runs first and only counts.
thread_local uint64_t lr_thread_faults = 0;
std::atomic<uint64_t> lr_all_faults{0};
// Page faults: time from the first handler (lr_count_fault) to the continue handler, which
// Windows calls when a handler resumed execution (the runtime's, for watched memory).
thread_local uint64_t lr_fault_start_us = 0;
thread_local uint64_t lr_thread_fault_us = 0;
std::atomic<uint64_t> lr_all_fault_us{0}, lr_fault_max_us{0}, lr_faults_timed{0};
#if defined(_WIN32)
LONG CALLBACK lr_fault_resumed(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && lr_fault_start_us) {
    uint64_t dt = lr_now_us() - lr_fault_start_us;
    lr_fault_start_us = 0;
    lr_thread_fault_us += dt;
    lr_all_fault_us.fetch_add(dt, std::memory_order_relaxed);
    lr_faults_timed.fetch_add(1, std::memory_order_relaxed);
    uint64_t prev = lr_fault_max_us.load(std::memory_order_relaxed);
    while (dt > prev && !lr_fault_max_us.compare_exchange_weak(prev, dt)) {
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#if defined(_WIN32)
LONG CALLBACK lr_count_fault(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    ++lr_thread_faults;
    lr_all_faults.fetch_add(1, std::memory_order_relaxed);
    lr_fault_start_us = lr_now_us();
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
// A crash nothing handled: where it happened (host code offset in the exe, matched against
// lightningreturns.map) and the game thread's guest state, logged before the process ends.
LPTOP_LEVEL_EXCEPTION_FILTER lr_previous_crash_filter = nullptr;
LONG WINAPI lr_crash_filter(EXCEPTION_POINTERS* info) {
  static std::atomic<int> once{0};
  if (once.fetch_add(1) == 0) {
    auto* rec = info->ExceptionRecord;
    uintptr_t rip = uintptr_t(rec->ExceptionAddress);
    uintptr_t exe = uintptr_t(GetModuleHandleW(nullptr));
    std::string what = fmt::format("exception {:08X}", uint32_t(rec->ExceptionCode));
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
      what = fmt::format("access violation ({} of host {:016X})", rec->ExceptionInformation[0] ? "write" : "read",
                         uint64_t(rec->ExceptionInformation[1]));
    std::string where = (rip >= exe && rip < exe + 0x10000000) ? fmt::format("exe+{:X}", rip - exe)
                                                                : fmt::format("{:016X} (outside the exe)", uint64_t(rip));
    const PPCContext* c = lr_my_waits().ctx.load(std::memory_order_relaxed);
    if (c) {
      std::string regs;
      decltype(&c->r3) rs[] = {&c->r3, &c->r4, &c->r5, &c->r6, &c->r7, &c->r8, &c->r9, &c->r10,
                                 &c->r11, &c->r12, &c->r28, &c->r29, &c->r30, &c->r31};
      const char* names[] = {"r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r28", "r29", "r30", "r31"};
      for (int r = 0; r < 14; ++r) regs += fmt::format(" {}={:08X}", names[r], rs[r]->u32);
      REXLOG_ERROR("LR crash: {} at {}; guest lr {:08X} r1 {:08X};{}; guest calls:{}", what, where,
                   uint32_t(c->lr), c->r1.u32, regs, lr_guest_stack(c->r1.u32));
    } else {
      REXLOG_ERROR("LR crash: {} at {} (not a game thread)", what, where);
    }
    ::rex::FlushLogging();
  }
  return lr_previous_crash_filter ? lr_previous_crash_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}
#endif
void lr_install_fault_counter() {
#if defined(_WIN32)
  static bool installed = false;
  if (!installed) {
    installed = true;
    AddVectoredExceptionHandler(1, lr_count_fault);
    AddVectoredContinueHandler(1, lr_fault_resumed);
    lr_previous_crash_filter = SetUnhandledExceptionFilter(lr_crash_filter);
  }
#endif
}
}  // namespace

namespace {
// A wait on one of the game's Mutex handles that does not come through Mutex::Lock (see "The
// game's Mutex class" below): checked and reported, never redirected (always false).
bool lr_foreign_mutex_wait(uint32_t fn, PPCContext& ctx, uint8_t* base);
}  // namespace

#define LR_TIMED_WAIT(addr, kind)                                   \
  REX_EXTERN(__imp__sub_##addr);                                    \
  REX_HOOK_RAW(sub_##addr) {                                        \
    if (lr_foreign_mutex_wait(0x##addr, ctx, base)) return;         \
    if (lr_wait_acc.depth++) {                                      \
      __imp__sub_##addr(ctx, base);                                 \
      --lr_wait_acc.depth;                                          \
      return;                                                       \
    }                                                               \
    uint32_t caller = uint32_t(ctx.lr);                             \
    uint64_t t0 = lr_now_us();                                      \
    auto& me = lr_my_waits();                                       \
    me.current_since_us.store(t0, std::memory_order_relaxed);       \
    me.current_fn.store(0x##addr, std::memory_order_relaxed);       \
    me.current_sp.store(ctx.r1.u32, std::memory_order_relaxed);     \
    me.ctx.store(&ctx, std::memory_order_relaxed);                  \
    me.current_site.store(caller, std::memory_order_release);       \
    __imp__sub_##addr(ctx, base);                                   \
    me.current_site.store(0, std::memory_order_release);            \
    lr_wait_note(kind, 0x##addr, caller, lr_now_us() - t0);         \
    --lr_wait_acc.depth;                                            \
  }

LR_TIMED_WAIT(82D328F0, kLrSleep)  // SleepEx
LR_TIMED_WAIT(82D4C0A8, kLrSleep)  // NtYieldExecution
LR_TIMED_WAIT(82D41248, kLrWait)   // WaitForSingleObject(Ex)
LR_TIMED_WAIT(82D32EC8, kLrWait)
LR_TIMED_WAIT(82D34038, kLrWait)
LR_TIMED_WAIT(82D3A038, kLrWait)   // WaitForMultipleObjects
LR_TIMED_WAIT(82D42B20, kLrWait)
LR_TIMED_WAIT(82D41C68, kLrWait)
LR_TIMED_WAIT(82D993F8, kLrWait)
LR_TIMED_WAIT(82D484F8, kLrWait)
LR_TIMED_WAIT(82D486A8, kLrWait)
LR_TIMED_WAIT(82D9D110, kLrWait)
LR_TIMED_WAIT(82D42A88, kLrWait)
LR_TIMED_WAIT(82D485D8, kLrWait)
LR_TIMED_WAIT(82D32CB8, kLrWait)
LR_TIMED_WAIT(82D429F0, kLrWait)
LR_TIMED_WAIT(82D3A650, kLrWait)
LR_TIMED_WAIT(82D42958, kLrWait)
LR_TIMED_WAIT(82D3B2F0, kLrWait)
LR_TIMED_WAIT(829CD360, kLrWait)

// ---------------------------------------------------------------------------------------
// Job system (sub_829E83F8): the main thread splits each frame phase into jobs, wakes the
// three worker threads with SetEvent (sub_82D32510, from 829E84A0), runs jobs itself, and then
// spins until the workers have cleared their bits. The workers sleep in WaitForSingleObject
// (sub_82D324D0, from 829E8678) on their own events.
//
// Kernel events go through the runtime's handle table, which every kernel call locks with the
// runtime's one global lock - the lock page faults, memory protection changes, and the renderer's
// shared-memory bookkeeping also hold. Step 30's report had the main thread inside SetEvent
// and spinning for workers that were still asleep in the slow frames. The worker events are
// host events instead (LR_JOB_EVENTS=kernel keeps the kernel's): a worker registers its event
// on its first wait (taking a signal already in the kernel event), SetEvent on a registered
// handle signals the host event, and a waiting worker still checks the kernel event every
// 250 ms in case something sets it another way.
//
// The job wrappers (sub_82AB4B60 plain, sub_82AB4B78 / sub_82AB4C40 timed; job function at
// +8) record which job a thread runs, for the slow-frame stacks.
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_82AB4B60);
REX_HOOK_RAW(sub_82AB4B60) {
  auto& me = lr_my_waits();
  uint32_t previous = me.job.exchange(lr_load_be32(base, ctx.r4.u32 + 8), std::memory_order_relaxed);
  __imp__sub_82AB4B60(ctx, base);
  me.job.store(previous, std::memory_order_relaxed);
}
REX_EXTERN(__imp__sub_82AB4B78);
REX_HOOK_RAW(sub_82AB4B78) {
  auto& me = lr_my_waits();
  uint32_t previous = me.job.exchange(lr_load_be32(base, ctx.r4.u32 + 8), std::memory_order_relaxed);
  __imp__sub_82AB4B78(ctx, base);
  me.job.store(previous, std::memory_order_relaxed);
}
REX_EXTERN(__imp__sub_82AB4C40);
REX_HOOK_RAW(sub_82AB4C40) {
  auto& me = lr_my_waits();
  uint32_t previous = me.job.exchange(lr_load_be32(base, ctx.r4.u32 + 8), std::memory_order_relaxed);
  __imp__sub_82AB4C40(ctx, base);
  me.job.store(previous, std::memory_order_relaxed);
}

namespace {
constexpr uint32_t kLrJobWorkerWaitSite = 0x829E8678;
bool lr_game_mutex_host();  // the game's Mutex class, below
struct LrGameMutex;
std::shared_ptr<LrGameMutex> lr_game_mutex(uint32_t handle);

bool lr_job_events_host() {
  static const bool host = [] {
    const char* v = std::getenv("LR_JOB_EVENTS");
    bool on = !(v && (v[0] == 'k' || v[0] == 'K'));
    REXLOG_INFO("LR job events: {}", on ? "host (worker wake-ups bypass the kernel)" : "kernel (LR_JOB_EVENTS=kernel)");
    return on;
  }();
  return host;
}

// Auto-reset event.
struct LrHostEvent {
#if defined(_WIN32)
  HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  ~LrHostEvent() { CloseHandle(h); }
  void Set() { SetEvent(h); }
  bool Wait(uint32_t ms) { return WaitForSingleObject(h, ms) == WAIT_OBJECT_0; }
#else
  std::mutex m;
  std::condition_variable cv;
  bool signaled = false;
  void Set() {
    {
      std::lock_guard<std::mutex> lock(m);
      signaled = true;
    }
    cv.notify_one();
  }
  bool Wait(uint32_t ms) {
    std::unique_lock<std::mutex> lock(m);
    if (!cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return signaled; })) return false;
    signaled = false;
    return true;
  }
#endif
};
std::mutex g_lr_job_events_mutex;
std::unordered_map<uint32_t, std::unique_ptr<LrHostEvent>> g_lr_job_events;
std::atomic<uint32_t> g_lr_job_events_count{0};
std::atomic<uint64_t> g_lr_job_host_sets{0}, g_lr_job_host_wakes{0}, g_lr_job_kernel_wakes{0};

LrHostEvent* lr_job_event(uint32_t handle) {
  if (!g_lr_job_events_count.load(std::memory_order_acquire)) return nullptr;
  std::lock_guard<std::mutex> lock(g_lr_job_events_mutex);
  auto it = g_lr_job_events.find(handle);
  return it == g_lr_job_events.end() ? nullptr : it->second.get();
}
}  // namespace

// SetEvent(handle)
REX_EXTERN(__imp__sub_82D32510);
REX_HOOK_RAW(sub_82D32510) {
  if (lr_job_events_host()) {
    if (LrHostEvent* ev = lr_job_event(ctx.r3.u32)) {
      ev->Set();
      g_lr_job_host_sets.fetch_add(1, std::memory_order_relaxed);
      ctx.r3.u64 = 1;
      return;
    }
  }
  __imp__sub_82D32510(ctx, base);
}

// WaitForSingleObject(handle, milliseconds)
REX_EXTERN(__imp__sub_82D324D0);
REX_HOOK_RAW(sub_82D324D0) {
  if (uint32_t(ctx.lr) != kLrJobWorkerWaitSite || !lr_job_events_host()) {
    __imp__sub_82D324D0(ctx, base);
    return;
  }
  uint32_t handle = ctx.r3.u32, timeout_ms = ctx.r4.u32;
  auto poll_kernel = [&]() {
    ctx.r3.u64 = handle;
    ctx.r4.u64 = 0;
    __imp__sub_82D324D0(ctx, base);
    return ctx.r3.u32 == 0;
  };
  LrHostEvent* ev = lr_job_event(handle);
  if (!ev) {
    {
      std::lock_guard<std::mutex> lock(g_lr_job_events_mutex);
      auto& slot = g_lr_job_events[handle];
      if (!slot) slot = std::make_unique<LrHostEvent>();
      ev = slot.get();
      g_lr_job_events_count.store(uint32_t(g_lr_job_events.size()), std::memory_order_release);
    }
    REXLOG_INFO("LR job events: worker event {:08X} now a host event", handle);
    if (poll_kernel()) return;  // set before it was registered
  }
  // Counted as a guest wait, like the timed wait wrappers.
  auto& me = lr_my_waits();
  uint64_t t0 = lr_now_us();
  me.current_since_us.store(t0, std::memory_order_relaxed);
  me.current_fn.store(0x82D324D0, std::memory_order_relaxed);
  me.current_sp.store(ctx.r1.u32, std::memory_order_relaxed);
  me.ctx.store(&ctx, std::memory_order_relaxed);
  me.current_site.store(kLrJobWorkerWaitSite, std::memory_order_release);
  uint32_t result = 0x102;  // STATUS_TIMEOUT
  uint64_t waited_ms = 0;
  for (;;) {
    uint32_t slice = 250;
    if (timeout_ms != 0xFFFFFFFFu) slice = uint32_t(std::min<uint64_t>(slice, timeout_ms - std::min<uint64_t>(waited_ms, timeout_ms)));
    if (ev->Wait(slice)) {
      g_lr_job_host_wakes.fetch_add(1, std::memory_order_relaxed);
      result = 0;
      break;
    }
    waited_ms += slice;
    bool kernel_set = poll_kernel();
    // The timed wait wrapper the poll went through cleared these: this thread still waits.
    me.current_fn.store(0x82D324D0, std::memory_order_relaxed);
    me.current_site.store(kLrJobWorkerWaitSite, std::memory_order_release);
    if (kernel_set) {
      g_lr_job_kernel_wakes.fetch_add(1, std::memory_order_relaxed);
      result = 0;
      break;
    }
    if (timeout_ms != 0xFFFFFFFFu && waited_ms >= timeout_ms) break;
  }
  me.current_site.store(0, std::memory_order_release);
  lr_wait_note(kLrWait, 0x82D324D0, kLrJobWorkerWaitSite, lr_now_us() - t0);
  ctx.r3.u64 = result;
}

// ---------------------------------------------------------------------------------------
// The game's Mutex class: its constructor sub_82625740 creates [obj] = CreateMutex(NULL, FALSE,
// "white%08x"); Lock sub_826257B0 = WaitForSingleObject([obj], INFINITE) (483 callers, plus
// sub_82626110), Unlock sub_826257C0 = ReleaseMutex([obj]) (853 callers). In town the main
// thread alone locks and unlocks these ~360 times a frame, two kernel calls each, and every
// kernel call takes the runtime's global lock (see the job system above). These mutexes are
// host locks instead - recursive and owned by a thread like the kernel's mutants - registered
// when the game creates them. LR_GAME_MUTEX=kernel keeps the kernel's.
// ---------------------------------------------------------------------------------------
namespace {
bool lr_game_mutex_host() {
  static const bool host = [] {
    // Off unless asked for: host locks hung the game in step 32 (PC logo, Android save load)
    // before the hand-off below existed, and the kernel mutants cost little.
    const char* v = std::getenv("LR_GAME_MUTEX");
    bool on = v && (v[0] == 'h' || v[0] == 'H');
    REXLOG_INFO("LR game mutexes: {}", on ? "host locks (LR_GAME_MUTEX=host)" : "kernel mutants");
    return on;
  }();
  return host;
}
struct LrGameMutex {
  std::mutex m;
  std::condition_variable cv;
  std::thread::id owner;
  uint32_t count = 0;
  // Threads waiting, in arrival order. A release hands the mutex to the first one, as the
  // kernel does: the game unlocks and immediately relocks some mutexes every frame, and a
  // waiter that only got woken would never get in (step 32 hung at the logo that way).
  std::deque<std::thread::id> queue;
  LrThreadWaits* owner_waits = nullptr;  // for the "waited 3 s" report
  // Free right now for this thread (caller holds m).
  bool FreeFor(std::thread::id me) const { return (count == 0 && queue.empty()) || (count && owner == me); }
  // false: had to wait
  bool Lock(uint32_t handle = 0) {
    auto me = std::this_thread::get_id();
    std::unique_lock<std::mutex> lock(m);
    if (FreeFor(me)) {
      if (count++ == 0) {
        owner = me;
        owner_waits = &lr_my_waits();
      }
      return true;
    }
    queue.push_back(me);
    auto mine = [&] { return count && owner == me; };
    // A wait this long is a deadlock or a stuck holder: say who holds it, once per wait.
    if (!cv.wait_for(lock, std::chrono::seconds(3), mine)) {
      std::string holder = " (unknown thread)";
      if (LrThreadWaits* w = owner_waits) {
        const PPCContext* c = w->ctx.load(std::memory_order_relaxed);
        uint32_t site = w->current_site.load(std::memory_order_relaxed);
        holder = fmt::format(" t{}{}{}", w->index, site ? fmt::format(" (in a wait from {:08X})", site) : std::string(),
                             c ? lr_guest_stack(c->r1.u32, 8) : std::string(" (no stack)"));
      }
      REXLOG_WARN("LR game mutexes: t{} waited 3 s for {:08X}, held by{}", lr_my_waits().index, handle, holder);
      cv.wait(lock, mine);
    }
    // Unlock made this thread the owner with count 1.
    owner_waits = &lr_my_waits();
    return false;
  }
  bool Unlock() {
    std::unique_lock<std::mutex> lock(m);
    if (!count || owner != std::this_thread::get_id()) return false;
    if (--count == 0) {
      owner_waits = nullptr;
      if (queue.empty()) {
        owner = std::thread::id();
        return true;
      }
      owner = queue.front();
      queue.pop_front();
      count = 1;
      lock.unlock();
      cv.notify_all();
    }
    return true;
  }
};
std::shared_mutex g_lr_game_mutexes_mutex;
std::unordered_map<uint32_t, std::shared_ptr<LrGameMutex>> g_lr_game_mutexes;
std::atomic<uint64_t> g_lr_game_mutex_locks{0}, g_lr_game_mutex_waits{0}, g_lr_game_mutex_wait_us{0};

std::shared_ptr<LrGameMutex> lr_game_mutex(uint32_t handle) {
  std::shared_lock<std::shared_mutex> lock(g_lr_game_mutexes_mutex);
  auto it = g_lr_game_mutexes.find(handle);
  return it == g_lr_game_mutexes.end() ? nullptr : it->second;
}
}  // namespace

namespace {
std::atomic<uint64_t> g_lr_game_mutex_foreign{0};
bool lr_foreign_mutex_wait(uint32_t fn, PPCContext& ctx, uint8_t* base) {
  if (!lr_game_mutex_host()) return false;
  {
    std::shared_lock<std::shared_mutex> lock(g_lr_game_mutexes_mutex);
    if (g_lr_game_mutexes.empty()) return false;
  }
  // The game also closes some of these handles without the destructor above, and the kernel
  // hands the number to a new object (an event, a semaphore). A handle we know that is no
  // longer a mutant is forgotten; a live one waited on another way is only reported - the
  // kernel mutant still works for those waiters. Nothing is redirected.
  auto check = [&](uint32_t handle) {
    if (!lr_game_mutex(handle)) return;
    bool mutant = false;
    if (auto* kernel_state = REX_KERNEL_STATE()) {
      auto object = kernel_state->object_table()->LookupObject<rex::system::XObject>(handle);
      mutant = object && object->type() == rex::system::XObject::Type::Mutant;
    }
    if (!mutant) {
      std::unique_lock<std::shared_mutex> lock(g_lr_game_mutexes_mutex);
      g_lr_game_mutexes.erase(handle);
      return;
    }
    g_lr_game_mutex_foreign.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 5)
      REXLOG_WARN("LR game mutexes: {:08X} waited on through sub_{:08X} from {:08X}, not Mutex::Lock", handle, fn,
                  uint32_t(ctx.lr));
  };
  if (fn == 0x82D3A038) {  // WaitForMultipleObjectsEx(count, handles, all, ms, alertable)
    uint32_t count = std::min<uint32_t>(ctx.r3.u32, 64), handles = ctx.r4.u32;
    for (uint32_t i = 0; handles && i < count; ++i) check(lr_load_be32(base, handles + 4 * i));
  } else {
    check(ctx.r3.u32);
  }
  return false;
}
}  // namespace

REX_EXTERN(__imp__sub_82625740);
REX_HOOK_RAW(sub_82625740) {
  uint32_t obj = ctx.r3.u32;
  __imp__sub_82625740(ctx, base);
  if (!lr_game_mutex_host()) return;
  uint32_t handle = lr_load_be32(base, obj);
  if (!handle) return;
  std::unique_lock<std::shared_mutex> lock(g_lr_game_mutexes_mutex);
  g_lr_game_mutexes[handle] = std::make_shared<LrGameMutex>();
}

// The destructor these handle holders share (sub_82625638: CloseHandle([obj]), [obj] = 0): the
// handle is forgotten before the kernel can give its number to another object.
REX_EXTERN(__imp__sub_82625638);
REX_HOOK_RAW(sub_82625638) {
  if (lr_game_mutex_host()) {
    uint32_t handle = lr_load_be32(base, ctx.r3.u32);
    std::unique_lock<std::shared_mutex> lock(g_lr_game_mutexes_mutex);
    if (handle) g_lr_game_mutexes.erase(handle);
  }
  __imp__sub_82625638(ctx, base);
}

REX_EXTERN(__imp__sub_826257B0);
REX_HOOK_RAW(sub_826257B0) {
  auto mutex = lr_game_mutex_host() ? lr_game_mutex(lr_load_be32(base, ctx.r3.u32)) : nullptr;
  if (!mutex) {
    __imp__sub_826257B0(ctx, base);
    return;
  }
  g_lr_game_mutex_locks.fetch_add(1, std::memory_order_relaxed);
  uint32_t caller = uint32_t(ctx.lr);
  // Uncontended: no wait to account for. Contended: counted as a guest wait at the caller.
  auto& me_waits = lr_my_waits();
  {
    std::unique_lock<std::mutex> probe(mutex->m, std::try_to_lock);
    if (probe.owns_lock() && mutex->FreeFor(std::this_thread::get_id())) {
      if (mutex->count == 0) {
        mutex->owner = std::this_thread::get_id();
        mutex->owner_waits = &me_waits;
      }
      ++mutex->count;
      ctx.r3.u64 = 0;
      return;
    }
  }
  auto& me = lr_my_waits();
  uint64_t t0 = lr_now_us();
  me.current_since_us.store(t0, std::memory_order_relaxed);
  me.current_fn.store(0x826257B0, std::memory_order_relaxed);
  me.current_sp.store(ctx.r1.u32, std::memory_order_relaxed);
  me.ctx.store(&ctx, std::memory_order_relaxed);
  me.current_site.store(caller, std::memory_order_release);
  mutex->Lock(lr_load_be32(base, ctx.r3.u32));
  me.current_site.store(0, std::memory_order_release);
  uint64_t us = lr_now_us() - t0;
  g_lr_game_mutex_waits.fetch_add(1, std::memory_order_relaxed);
  g_lr_game_mutex_wait_us.fetch_add(us, std::memory_order_relaxed);
  lr_wait_note(kLrWait, 0x826257B0, caller, us);
  ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__sub_826257C0);
REX_HOOK_RAW(sub_826257C0) {
  auto mutex = lr_game_mutex_host() ? lr_game_mutex(lr_load_be32(base, ctx.r3.u32)) : nullptr;
  if (!mutex) {
    __imp__sub_826257C0(ctx, base);
    return;
  }
  if (!mutex->Unlock()) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 5)
      REXLOG_WARN("LR game mutexes: {:08X} released by a thread that does not own it", lr_load_be32(base, ctx.r3.u32));
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r3.u64 = 1;
}

// ---------------------------------------------------------------------------------------
// Global lock probe: every 2 ms a host thread takes and releases the runtime's global lock and
// times how long it had to wait. A long wait means something held the lock that long - and
// every guest kernel call (waits, events, allocations) waits for it too. When the lock is
// busy the probe notes what the game threads are running at that moment (the holder is among
// them unless it is the renderer). Reported per slow frame and every 10 s.
// ---------------------------------------------------------------------------------------
namespace {
// OS name of a thread of this process ("" if unknown).
std::string lr_os_thread_name(int tid) {
  std::string name;
#if defined(_WIN32)
  if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, DWORD(tid))) {
    // Looked up at run time: the declaration depends on the Windows SDK version targeted.
    using GetDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static auto get_desc = reinterpret_cast<GetDescFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    PWSTR desc = nullptr;
    if (get_desc && SUCCEEDED(get_desc(h, &desc)) && desc) {
      for (const wchar_t* c = desc; *c && name.size() < 40; ++c) name += (*c < 128) ? char(*c) : '?';
      LocalFree(desc);
    }
    CloseHandle(h);
  }
#else
  char path[64], buf[32] = {};
  snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
  if (FILE* f = fopen(path, "r")) {
    if (fgets(buf, sizeof(buf), f)) buf[strcspn(buf, "\n")] = 0;
    fclose(f);
    name = buf;
  }
#endif
  return name;
}

// A thread for reports: tN plus its guest call chain for game threads, else its OS name.
std::string lr_thread_label(int tid) {
  {
    std::lock_guard<std::mutex> lock(lr_thread_waits_mutex);
    for (auto& t : lr_thread_waits) {
      if (t->tid != tid || t->dead.load(std::memory_order_relaxed)) continue;
      std::string out = fmt::format("t{}", t->index);
      if (const PPCContext* tc = t->ctx.load(std::memory_order_relaxed)) out += lr_guest_stack(tc->r1.u32, 4);
      return out;
    }
  }
  return fmt::format("host {} '{}'", tid, lr_os_thread_name(tid));
}

#if defined(__GLIBC__)
// Linux: who holds the global lock. The owner's thread id is read from the pthread mutex and
// that thread is asked (by a signal) for its native call stack.
std::atomic<int> g_lr_bt_state{0};  // 0 idle, 1 asked, 2 taken
void* g_lr_bt_frames[32];
int g_lr_bt_count = 0;
void lr_bt_signal(int) {
  if (g_lr_bt_state.load(std::memory_order_acquire) != 1) return;
  g_lr_bt_count = backtrace(g_lr_bt_frames, 32);
  g_lr_bt_state.store(2, std::memory_order_release);
}
int lr_bt_signo() { return SIGRTMAX - 2; }
std::string lr_symbolize(void* pc) {
  Dl_info info{};
  if (!dladdr(pc, &info) || !info.dli_fname) return fmt::format("{}", pc);
  const char* mod = strrchr(info.dli_fname, '/');
  mod = mod ? mod + 1 : info.dli_fname;
  uintptr_t off = uintptr_t(pc) - uintptr_t(info.dli_fbase);
  if (info.dli_sname) return fmt::format("{}:{}+{:X}", mod, info.dli_sname, uintptr_t(pc) - uintptr_t(info.dli_saddr));
  return fmt::format("{}+{:X}", mod, off);
}
// Owner's call stack, or "" if it could not be taken.
std::string lr_owner_stack(int tid) {
  int expected = 0;
  if (!g_lr_bt_state.compare_exchange_strong(expected, 1)) return "";
  if (syscall(SYS_tgkill, getpid(), tid, lr_bt_signo()) != 0) {
    g_lr_bt_state.store(0);
    return "";
  }
  for (int i = 0; i < 200 && g_lr_bt_state.load(std::memory_order_acquire) != 2; ++i)
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  std::string out;
  if (g_lr_bt_state.load(std::memory_order_acquire) == 2) {
    // Frame 0 is the signal handler, 1 the kernel's signal trampoline.
    for (int i = 2, n = 0; i < g_lr_bt_count && n < 12; ++i, ++n) out += " < " + lr_symbolize(g_lr_bt_frames[i]);
    g_lr_bt_state.store(0, std::memory_order_release);
  }
  // A late signal finds state != 1 and does nothing.
  return out;
}
#elif defined(_WIN32)
// Windows: the owner's thread id is read from the MSVC STL mutex (_Mtx_internal_imp_t: _Type,
// 64-byte critical-section storage, _Thread_id at offset 72). The offset is checked at start
// while the probe holds the lock itself; if it does not match, owners are not reported.
bool g_lr_owner_layout_ok = false;
long lr_mutex_owner(std::recursive_mutex& m) {
  return *reinterpret_cast<volatile long*>(reinterpret_cast<char*>(&m) + 72);
}
#endif

struct LrLockProbe {
  std::atomic<uint64_t> frame_max_us{0};
  std::atomic<uint64_t> over2{0}, over8{0}, over30{0}, max_us{0}, busy_us{0};
  std::mutex mutex;
  std::unordered_map<std::string, uint32_t> holders;  // running guest stacks during waits > 4 ms
  std::unordered_map<std::string, uint32_t> owners;   // lock owner (Linux: with its native stack)
  bool started = false;

  void Start() {
    if (started) return;
    started = true;
#if defined(__GLIBC__)
    {
      void* warm[4];
      backtrace(warm, 4);  // loads the unwinder outside a signal handler
      struct sigaction sa{};
      sa.sa_handler = lr_bt_signal;
      sa.sa_flags = SA_RESTART;
      sigemptyset(&sa.sa_mask);
      sigaction(lr_bt_signo(), &sa, nullptr);
    }
#elif defined(_WIN32)
    {
      auto& global = rex::thread::global_critical_region::mutex();
      global.lock();
      g_lr_owner_layout_ok = lr_mutex_owner(global) == long(GetCurrentThreadId());
      global.unlock();
      REXLOG_INFO("LR global lock: owner thread {}", g_lr_owner_layout_ok ? "readable" : "not readable (unknown mutex layout)");
    }
#endif
    std::thread([this] {
      auto& global = rex::thread::global_critical_region::mutex();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        uint64_t t0 = lr_now_us();
        std::vector<std::string> running;
        if (global.try_lock()) {
          global.unlock();
          continue;
        }
        // Busy right now: what the game threads run (only kept if the wait turns out long).
        {
          std::vector<std::shared_ptr<LrThreadWaits>> threads;
          {
            std::lock_guard<std::mutex> lock(lr_thread_waits_mutex);
            threads = lr_thread_waits;
          }
          for (auto& t : threads) {
            if (t->dead.load(std::memory_order_relaxed)) continue;
            uint32_t site = t->current_site.load(std::memory_order_relaxed);
            const PPCContext* tc = t->ctx.load(std::memory_order_relaxed);
            if (!tc) continue;
            // A thread inside a kernel wait call may be the one waiting for the lock too;
            // listed with its wait site so holders and waiters can be told apart.
            std::string chain = fmt::format(" t{}:", t->index);
            if (site) chain += fmt::format(" (in wait from {:08X})", site);
            else chain += lr_guest_stack(tc->r1.u32, 4);
            if (uint32_t job = t->job.load(std::memory_order_relaxed)) chain += fmt::format(" [job {:08X}]", job);
            if (!site) running.push_back(std::move(chain));
          }
        }
        // Still busy after 3 ms: note who owns it (Linux also its native call stack).
        std::string owner_stack;
        for (;;) {
          if (global.try_lock()) break;
          if (owner_stack.empty() && lr_now_us() - t0 > 3000) {
#if defined(__GLIBC__)
            int tid = global.native_handle()->__data.__owner;
            if (tid > 0) owner_stack = " owner " + lr_thread_label(tid) + ":" + lr_owner_stack(tid);
#elif defined(_WIN32)
            int tid = g_lr_owner_layout_ok ? int(lr_mutex_owner(global)) : 0;
            if (tid > 0) owner_stack = " owner " + lr_thread_label(tid);
#endif
            if (owner_stack.empty()) owner_stack = " owner unknown";
          }
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        global.unlock();
        if (!owner_stack.empty() && lr_now_us() - t0 > 4000) {
          std::lock_guard<std::mutex> lock(mutex);
          if (owners.size() < 256) ++owners[owner_stack];
        }
        uint64_t waited = lr_now_us() - t0;
        uint64_t prev = frame_max_us.load(std::memory_order_relaxed);
        while (waited > prev && !frame_max_us.compare_exchange_weak(prev, waited)) {
        }
        prev = max_us.load(std::memory_order_relaxed);
        while (waited > prev && !max_us.compare_exchange_weak(prev, waited)) {
        }
        if (waited > 1000) busy_us.fetch_add(waited, std::memory_order_relaxed);
        if (waited > 2000) over2.fetch_add(1, std::memory_order_relaxed);
        if (waited > 8000) over8.fetch_add(1, std::memory_order_relaxed);
        if (waited > 30000) over30.fetch_add(1, std::memory_order_relaxed);
        if (waited > 4000) {
          std::lock_guard<std::mutex> lock(mutex);
          if (running.empty()) running.push_back(" (no game thread running: renderer or a host thread)");
          for (auto& r : running)
            if (holders.size() < 256) ++holders[r];
        }
      }
    }).detach();
  }

  std::string TakeWindow() {
    std::vector<std::pair<std::string, uint32_t>> v;
    {
      std::lock_guard<std::mutex> lock(mutex);
      v.assign(holders.begin(), holders.end());
      holders.clear();
    }
    std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
    std::vector<std::pair<std::string, uint32_t>> o;
    {
      std::lock_guard<std::mutex> lock(mutex);
      o.assign(owners.begin(), owners.end());
      owners.clear();
    }
    std::sort(o.begin(), o.end(), [](auto& x, auto& y) { return x.second > y.second; });
    std::string top;
    for (size_t i = 0; i < v.size() && i < 6; ++i) top += fmt::format("{}{}x{}", i ? "; " : "", v[i].second, v[i].first);
    std::string out = fmt::format(
        "probe waited over 2 ms {} times, over 8 ms {}, over 30 ms {}, longest {:.1f} ms, {} ms in waits over 1 ms",
        over2.exchange(0), over8.exchange(0), over30.exchange(0), max_us.exchange(0) / 1000.0, busy_us.exchange(0) / 1000);
    if (!top.empty()) out += "; running during long waits:" + top;
    for (size_t i = 0; i < o.size() && i < 4; ++i) out += fmt::format("\n    lock owner during a long wait ({}x):{}", o[i].second, o[i].first);
    return out;
  }
} g_lr_lock_probe;

}  // namespace

namespace {
// CPU use of every live game thread and of the whole process over the last 10 s.
std::string lr_cpu_report(uint64_t window_us) {
  static uint64_t last_process_us = 0;
  uint64_t process_us = 0;
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    auto ft = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 10; };
    process_us = ft(kernel) + ft(user);
  }
#else
  timespec ts{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) process_us = uint64_t(ts.tv_sec) * 1000000 + uint64_t(ts.tv_nsec) / 1000;
#endif
  std::vector<std::pair<uint64_t, int>> use;
  std::unordered_map<int, int> tids;  // index -> OS thread id
  std::vector<std::shared_ptr<LrThreadWaits>> threads;
  {
    std::lock_guard<std::mutex> lock(lr_thread_waits_mutex);
    threads = lr_thread_waits;
  }
  uint64_t guest_total = 0;
  for (auto& t : threads) {
    if (t->dead.load()) continue;
    uint64_t now = lr_thread_cpu_us(*t);
    uint64_t d = now >= t->last_cpu_us ? now - t->last_cpu_us : 0;
    bool first = t->last_cpu_us == 0;
    t->last_cpu_us = now;
    if (first) continue;
    guest_total += d;
    use.emplace_back(d, t->index);
    tids[t->index] = t->tid;
  }
  std::sort(use.begin(), use.end(), std::greater<>());
  std::string out = fmt::format("process {:.0f}% of one core ({} logical cores), game threads {:.0f}%:",
                                last_process_us ? 100.0 * double(process_us - last_process_us) / double(window_us) : 0.0,
                                std::thread::hardware_concurrency(), 100.0 * double(guest_total) / double(window_us));
  last_process_us = process_us;
  for (size_t i = 0; i < use.size() && i < 10; ++i) {
    out += fmt::format(" t{} {:.0f}%", use[i].second, 100.0 * double(use[i].first) / double(window_us));
    // Names of the busiest threads (the runtime names its own: audio, XMA, GPU...).
    if (i < 3 && use[i].first * 10 > window_us) {
      std::string name = lr_os_thread_name(tids[use[i].second]);
      if (!name.empty()) out += " '" + name + "'";
    }
  }
  return out;
}
}  // namespace

// ---------------------------------------------------------------------------------------
// Where the main thread's "game code" time goes in slow frames. Once a frame has run 30 ms, a
// sampler thread reads the main thread's guest call chain (lr_guest_stack) every millisecond
// while the main thread is not in a guest wait. Each "LR slow frame" line ends with the most
// frequent chains of that frame (count x return addresses, innermost first), and every 10 s
// "LR main thread busy in" sums them over all slow frames.
// ---------------------------------------------------------------------------------------
namespace {
struct LrMainSampler {
  std::atomic<uint64_t> frame_start_us{0};
  std::atomic<LrThreadWaits*> main{nullptr};
  std::mutex mutex;
  std::unordered_map<std::string, uint32_t> frame, window;
  // The other game threads at the same moments (those not in a guest wait): what the main
  // thread is waiting for when its slow frame is the job system's wait (829E8538).
  std::unordered_map<std::string, uint32_t> others_frame, others_window;
  bool started = false;

  void Start() {
    if (started) return;
    started = true;
    std::thread([this] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        uint64_t start = frame_start_us.load(std::memory_order_relaxed);
        LrThreadWaits* w = main.load(std::memory_order_relaxed);
        if (!start || !w || lr_now_us() - start < 30000) continue;
        if (w->current_site.load(std::memory_order_relaxed)) continue;  // in a guest wait
        const PPCContext* c = w->ctx.load(std::memory_order_relaxed);
        if (!c) continue;
        std::string chain = lr_guest_stack(c->r1.u32, 5);
        if (uint32_t job = w->job.load(std::memory_order_relaxed)) chain += fmt::format(" [job {:08X}]", job);
        std::vector<std::string> other_chains;
        {
          std::vector<std::shared_ptr<LrThreadWaits>> threads;
          {
            std::lock_guard<std::mutex> lock(lr_thread_waits_mutex);
            threads = lr_thread_waits;
          }
          for (auto& t : threads) {
            if (t.get() == w || t->dead.load(std::memory_order_relaxed) ||
                t->current_site.load(std::memory_order_relaxed))
              continue;
            const PPCContext* tc = t->ctx.load(std::memory_order_relaxed);
            if (!tc) continue;
            std::string other = fmt::format(" t{}:{}", t->index, lr_guest_stack(tc->r1.u32, 5));
            if (uint32_t job = t->job.load(std::memory_order_relaxed)) other += fmt::format(" [job {:08X}]", job);
            other_chains.push_back(std::move(other));
          }
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (frame.size() < 256) ++frame[chain];
        for (auto& o : other_chains)
          if (others_frame.size() < 512) ++others_frame[o];
      }
    }).detach();
  }

  static std::string Top(const std::unordered_map<std::string, uint32_t>& counts, size_t n) {
    std::vector<std::pair<std::string, uint32_t>> v(counts.begin(), counts.end());
    std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
    std::string out;
    for (size_t i = 0; i < v.size() && i < n; ++i) out += fmt::format("{}{}x{}", i ? "; " : "", v[i].second, v[i].first);
    return out;
  }

  // Main thread, after each frame.
  std::string TakeFrame(bool slow) {
    std::lock_guard<std::mutex> lock(mutex);
    std::string top;
    if (slow) {
      for (auto& e : frame) window[e.first] += e.second;
      for (auto& e : others_frame) others_window[e.first] += e.second;
      top = Top(frame, 3);
      if (!others_frame.empty()) top += " | other threads:" + Top(others_frame, 6);
    }
    frame.clear();
    others_frame.clear();
    return top;
  }

  std::string TakeWindow() {
    std::lock_guard<std::mutex> lock(mutex);
    std::string top = Top(window, 6);
    if (!others_window.empty()) top += " | other threads:" + Top(others_window, 12);
    window.clear();
    others_window.clear();
    return top;
  }
} g_lr_main_sampler;
}  // namespace

REX_EXTERN(__imp__sub_829CB8F0);
REX_HOOK_RAW(sub_829CB8F0) {
  g_lr_frames.fetch_add(1, std::memory_order_relaxed);
  lr_install_fault_counter();
  g_lr_guest_base.store(base, std::memory_order_relaxed);
  lr_apply_voice(base);
  lr_apply_aa();
  lr_cheats_frame(base);
  {
    auto& me = lr_my_waits();
    me.ctx.store(&ctx, std::memory_order_relaxed);
    g_lr_main_sampler.main.store(&me, std::memory_order_relaxed);
    g_lr_main_sampler.Start();
    g_lr_lock_probe.Start();
  }
  static uint64_t last_us = 0, next_summary_us = 0, last_faults = 0, last_all_faults = 0;
  static uint64_t last_cpu_us = 0, last_fault_us = 0, last_all_fault_us = 0, last_faults_timed = 0;
  static uint64_t window_cpu_us = 0, window_start_us = 0;
  uint64_t t0 = lr_now_us();
  __imp__sub_829CB8F0(ctx, base);
  uint64_t t1 = lr_now_us();
  auto& a = lr_wait_acc;
  auto& t = lr_main_totals;
  if (last_us) {
    uint64_t frame = t1 - last_us, present = t1 - t0;
    uint64_t waited = a.us[kLrSleep] + a.us[kLrWait] + a.us[kLrRenderer] + present;
    uint64_t code = frame > waited ? frame - waited : 0;
    uint64_t faults = lr_thread_faults - last_faults;
    t.faults += faults;
    t.fault_us += lr_thread_fault_us - last_fault_us;
    t.frames++;
    t.frame_us += frame;
    t.present_us += present;
    for (int k = 0; k < kLrKinds; ++k) t.us[k] += a.us[k];
#if defined(__ANDROID__)
    // Frame work (without the frame-pacing sleep) against the frame target, for Android's CPU
    // clock and core placement (android/game/lr_android_main.cpp).
    LrAndroidFrameDone(frame > a.us[kLrSleep] ? frame - a.us[kLrSleep] : 0,
                       g_lr_frame_rate_asked.load(std::memory_order_relaxed));
#endif
    std::string busy = g_lr_main_sampler.TakeFrame(frame > 45000);
    uint64_t cpu_now = lr_thread_cpu_us(lr_my_waits());
    uint64_t cpu = last_cpu_us && cpu_now >= last_cpu_us ? cpu_now - last_cpu_us : 0;
    window_cpu_us += cpu;
    uint64_t fault_us = lr_thread_fault_us - last_fault_us;
    uint64_t lock_max = g_lr_lock_probe.frame_max_us.exchange(0, std::memory_order_relaxed);
    if (frame > 45000) {
      t.slow++;
      if (t.slow <= 30 || t.slow % 10 == 0) {
        REXLOG_INFO(
            "LR slow frame: {} ms = game code {} ms + sleeping {} ms ({}x) + waiting {} ms ({}x) + "
            "renderer {} ms + present {} ms; slowest wait {} ms in sub_{:08X} called from {:08X}; "
            "main thread on a CPU {} ms; {} page faults on this thread ({} ms); global lock waits up to "
            "{} ms; busy in:{}",
            frame / 1000, code / 1000, a.us[kLrSleep] / 1000, a.count[kLrSleep], a.us[kLrWait] / 1000,
            a.count[kLrWait], a.us[kLrRenderer] / 1000, present / 1000, a.longest_us / 1000, a.longest_fn,
            a.longest_lr, cpu / 1000, faults, fault_us / 1000, lock_max / 1000,
            busy.empty() ? std::string(" -") : " " + busy);
      }
    }
  }
  g_lr_main_sampler.frame_start_us.store(t1, std::memory_order_relaxed);
  if (t1 >= next_summary_us) {
    if (next_summary_us && t.frames) {
      uint64_t waited = t.us[kLrSleep] + t.us[kLrWait] + t.us[kLrRenderer] + t.present_us;
      uint64_t all_faults = lr_all_faults.load(std::memory_order_relaxed);
      REXLOG_INFO(
          "LR main thread (10 s): {} frames, {} over 45 ms; per frame {:.1f} ms = game code {:.1f} + "
          "sleeping {:.1f} + waiting {:.1f} + renderer {:.1f} + present {:.1f}; page faults per frame: "
          "{:.1f} on the main thread, {:.1f} in all threads",
          t.frames, t.slow, t.frame_us / 1000.0 / t.frames,
          (t.frame_us > waited ? t.frame_us - waited : 0) / 1000.0 / t.frames, t.us[kLrSleep] / 1000.0 / t.frames,
          t.us[kLrWait] / 1000.0 / t.frames, t.us[kLrRenderer] / 1000.0 / t.frames, t.present_us / 1000.0 / t.frames,
          double(t.faults) / t.frames, double(all_faults - last_all_faults) / t.frames);
      last_all_faults = all_faults;
      std::string busy = g_lr_main_sampler.TakeWindow();
      if (!busy.empty()) REXLOG_INFO("LR main thread busy in (slow frames, 10 s): {}", busy);
      uint64_t window = t1 - window_start_us;
      uint64_t all_fault_us = lr_all_fault_us.load(), faults_timed = lr_faults_timed.load();
      REXLOG_INFO("LR CPU (10 s): main thread on a CPU {:.1f} ms per frame (game code {:.1f}); {}",
                  window_cpu_us / 1000.0 / t.frames,
                  (t.frame_us > waited ? t.frame_us - waited : 0) / 1000.0 / t.frames, lr_cpu_report(window));
      REXLOG_INFO("LR page faults (10 s): {} timed, {} ms in all threads ({} ms on the main thread), slowest {:.1f} ms",
                  faults_timed - last_faults_timed, (all_fault_us - last_all_fault_us) / 1000,
                  t.fault_us / 1000, lr_fault_max_us.exchange(0) / 1000.0);
      last_all_fault_us = all_fault_us;
      last_faults_timed = faults_timed;
      REXLOG_INFO("LR global lock (10 s): {}", g_lr_lock_probe.TakeWindow());
      if (lr_game_mutex_host())
        REXLOG_INFO("LR game mutexes (10 s): {} host locks, {} locks, {} had to wait ({} ms), {} waits not through "
                    "Mutex::Lock",
                    [] {
                      std::shared_lock<std::shared_mutex> lock(g_lr_game_mutexes_mutex);
                      return g_lr_game_mutexes.size();
                    }(),
                    g_lr_game_mutex_locks.exchange(0), g_lr_game_mutex_waits.exchange(0),
                    g_lr_game_mutex_wait_us.exchange(0) / 1000, g_lr_game_mutex_foreign.exchange(0));
      if (lr_job_events_host())
        REXLOG_INFO("LR job events (10 s): {} host events, {} sets, {} wake-ups by host event, {} by the kernel event",
                    g_lr_job_events_count.load(), g_lr_job_host_sets.exchange(0), g_lr_job_host_wakes.exchange(0),
                    g_lr_job_kernel_wakes.exchange(0));
    }
    // Every thread that spent over 0.5 s of the last 10 s in guest waits: its top wait sites.
    int main_index = lr_my_waits().index;
    std::vector<std::shared_ptr<LrThreadWaits>> threads;
    {
      std::lock_guard<std::mutex> lock(lr_thread_waits_mutex);
      threads = lr_thread_waits;
    }
    for (auto& w : threads) {
      std::vector<std::pair<uint32_t, std::pair<uint64_t, uint32_t>>> sites;
      {
        std::lock_guard<std::mutex> lock(w->mutex);
        sites.assign(w->sites.begin(), w->sites.end());
        w->sites.clear();
      }
      uint64_t total = 0;
      for (auto& e : sites) total += e.second.first;
      if (total < 500000 || !next_summary_us) continue;
      std::sort(sites.begin(), sites.end(), [](auto& x, auto& y) { return x.second.first > y.second.first; });
      std::string top;
      for (size_t i = 0; i < sites.size() && i < 4; ++i)
        top += fmt::format("{}{:08X} {} ms ({}x)", i ? ", " : "", sites[i].first, sites[i].second.first / 1000,
                           sites[i].second.second);
      REXLOG_INFO("LR waits (10 s): {} thread {} waited {} ms: {}", w->index == main_index ? "main" : "game",
                  w->index, total / 1000, top);
    }
    t = LrFrameTotals();
    window_cpu_us = 0;
    window_start_us = t1;
    next_summary_us = t1 + 10000000;
  }
  a.reset();
  last_us = t1;
  last_faults = lr_thread_faults;
  last_cpu_us = lr_thread_cpu_us(lr_my_waits());
  last_fault_us = lr_thread_fault_us;
}


// ---------------------------------------------------------------------------------------
// sub_829C9810 is the game's GPU wait step (D3D): it checks the command processor's
// write-back once and returns 1 to be called again, 0 to give up. On the console it starts
// with cctpl / db16cyc x32 / cctpm, which lower the waiting hardware thread's priority so the
// other thread on the core runs; the recompiled code drops those, so every GPU wait in the
// game was a full-speed busy loop competing with the renderer's own thread for a core (the
// slow frames' "busy in 829C4778 ... / 829C4DC0 ..." stacks). The first polls only pause
// (a wait usually ends within microseconds); after that the thread yields its time slice
// between polls. LR_GPU_WAIT_YIELD=0 restores the plain spin.
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_829C9810);
REX_HOOK_RAW(sub_829C9810) {
  static const bool yield = [] {
    const char* v = std::getenv("LR_GPU_WAIT_YIELD");
    return !(v && v[0] == '0');
  }();
  thread_local uint32_t wait_state = 0, polls = 0;
  uint32_t state = ctx.r3.u32;
  __imp__sub_829C9810(ctx, base);
  if (!yield || !ctx.r3.u32) {
    polls = 0;
    return;
  }
  if (state != wait_state) {
    wait_state = state;
    polls = 0;
  }
  if (++polls < 64) {
#if defined(_M_X64) || defined(__x86_64__)
    for (int i = 0; i < 16; ++i) _mm_pause();
#endif
  } else {
    std::this_thread::yield();
  }
}

// sub_829C4720 is the other GPU wait (D3D kickoff, through sub_829C6108): it waits for the
// command processor to finish a ring segment. Its time counts as "renderer" in the frame
// report, like sub_829C4D28 below, instead of game code.
REX_EXTERN(__imp__sub_829C4720);
REX_HOOK_RAW(sub_829C4720) {
  uint64_t t0 = lr_now_us();
  __imp__sub_829C4720(ctx, base);
  lr_wait_note(kLrRenderer, 0x829C4720, uint32_t(ctx.lr), lr_now_us() - t0);
}

// ---------------------------------------------------------------------------------------
// sub_82D3AC88 is the game's memcpy for 256 bytes and up (XMemCpy, or a write-combined copy
// for physical memory). The game moves its GPU resources around with it while streaming
// (sub_82AB4DC0, memmove, from the render code: tens of MB per frame during area loads).
// Every first write to memory the renderer watches is an access violation + page
// unprotect, 135-330 of them a frame in the user's slow frames ("CPU writes to watched
// memory"). A large copy into physical memory now tells the renderer about the whole
// destination at once, which unwatches it in one go. LR_COPY_INVALIDATE=0 disables it.
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_82D3AC88);
REX_HOOK_RAW(sub_82D3AC88) {
  static const bool enabled = [] {
    const char* v = std::getenv("LR_COPY_INVALIDATE");
    return !(v && v[0] == '0');
  }();
  uint32_t dst = ctx.r3.u32, size = ctx.r5.u32;
  if (enabled && size >= 0x10000 && dst >= 0xA0000000u && size <= 0x20000000u - (dst & 0x1FFFFFFFu)) {
    if (auto* kernel_state = REX_KERNEL_STATE()) {
      if (auto* memory = kernel_state->memory()) {
        memory->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                               dst, size, true, true);
      }
    }
  }
  __imp__sub_82D3AC88(ctx, base);
}

// The copy loops themselves: sub_82D3ABD8 (any size; from 256 bytes on, 128-byte VMX blocks
// with cache hints, called by sub_82D3AC88) and sub_82D1ED20 (the small memcpy: byte and word
// moves). Translated, those loops were the busiest code in the user's slow frames (the copy
// called from sub_82AB4DC0 while areas stream in). Both are plain copies that return the
// destination - the cache hints do nothing here - so the host's memmove does the same faster.
// LR_NATIVE_MEMCPY=0 keeps the game's loops.
namespace {
bool lr_native_memcpy() {
  static const bool enabled = [] {
    const char* v = std::getenv("LR_NATIVE_MEMCPY");
    return !(v && v[0] == '0');
  }();
  return enabled;
}
}  // namespace

#define LR_COPY_TIMER(size)

// A large copy into physical memory the renderer may watch: one notification for the whole
// destination instead of an access violation per page, as sub_82D3AC88 above does. Only from
// 64 KB: the notification takes the runtime's global lock even where nothing is watched, and
// the audio thread copies its (unwatched) buffers in a few KB at a time - step 31 did this from
// 4 KB and audio started popping while the game's page faults held that lock. Smaller copies
// into watched pages take the normal access-violation path.
void lr_copy_unwatch(uint32_t dst, uint32_t size) {
  if (size < 0x10000 || dst < 0xA0000000u || size > 0x20000000u - (dst & 0x1FFFFFFFu)) return;
  if (auto* kernel_state = REX_KERNEL_STATE()) {
    if (auto* memory = kernel_state->memory()) {
      memory->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(), dst, size,
                                             true, true);
    }
  }
}

// Overlapping ranges: the game's copies run forward and zero destination cache lines ahead of
// their stores (dcbzl), so they give other bytes than memmove there. Such calls run the game's
// own copy.
bool lr_forward_overlap(uint32_t dst, uint32_t src, uint32_t size, uint32_t fn, uint32_t caller) {
  if (!size || dst >= uint64_t(src) + size || src >= uint64_t(dst) + size) return false;
  (void)fn;
  (void)caller;
  return true;
}

REX_EXTERN(__imp__sub_82D3ABD8);
REX_HOOK_RAW(sub_82D3ABD8) {
  LR_COPY_TIMER(ctx.r5.u32);
  if (!lr_native_memcpy()) {
    __imp__sub_82D3ABD8(ctx, base);
    return;
  }
  uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, size = ctx.r5.u32;
  if (lr_forward_overlap(dst, src, size, 0x82D3ABD8, uint32_t(ctx.lr))) {
    __imp__sub_82D3ABD8(ctx, base);
    return;
  }
  lr_copy_unwatch(dst, size);
  if (size) std::memmove(base + dst, base + src, size);
  ctx.r3.u64 = dst;
}

// sub_82D3AB28: the third copy (head and tail through sub_82D1ED20, the middle in 128-byte
// VMX blocks, sub_82D49170 / sub_82D49494). Step 30's slow frames had a streaming thread (t4,
// from sub_8238C408 under the game lock sub_82643FD8) in it most of the time.
REX_EXTERN(__imp__sub_82D3AB28);
REX_HOOK_RAW(sub_82D3AB28) {
  LR_COPY_TIMER(ctx.r5.u32);
  if (!lr_native_memcpy()) {
    __imp__sub_82D3AB28(ctx, base);
    return;
  }
  uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, size = ctx.r5.u32;
  if (lr_forward_overlap(dst, src, size, 0x82D3AB28, uint32_t(ctx.lr))) {
    __imp__sub_82D3AB28(ctx, base);
    return;
  }
  lr_copy_unwatch(dst, size);
  if (size) std::memmove(base + dst, base + src, size);
  ctx.r3.u64 = dst;
}

// sub_82D3AD10: a fourth copy (62 callers): up to 16 bytes in doubleword/word/byte moves, then
// 16-byte VMX moves with dcbzl on the destination lines. Forward only; returns the destination.
REX_EXTERN(__imp__sub_82D3AD10);
REX_HOOK_RAW(sub_82D3AD10) {
  LR_COPY_TIMER(ctx.r5.u32);
  // The sound engine's copy (most of its 62 callers). Step 31 made it native and audio popped;
  // it is not on any slow path, so it runs as game code unless LR_NATIVE_COPY_AD10=1.
  static const bool native = [] {
    const char* v = std::getenv("LR_NATIVE_COPY_AD10");
    return v && v[0] == '1';
  }();
  if (!native || !lr_native_memcpy()) {
    __imp__sub_82D3AD10(ctx, base);
    return;
  }
  uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, size = ctx.r5.u32;
  if (lr_forward_overlap(dst, src, size, 0x82D3AD10, uint32_t(ctx.lr))) {
    __imp__sub_82D3AD10(ctx, base);
    return;
  }
  lr_copy_unwatch(dst, size);
  if (size) std::memmove(base + dst, base + src, size);
  ctx.r3.u64 = dst;
}

REX_EXTERN(__imp__sub_82D1ED20);
REX_HOOK_RAW(sub_82D1ED20) {
  LR_COPY_TIMER(ctx.r5.u32);
  if (!lr_native_memcpy()) {
    __imp__sub_82D1ED20(ctx, base);
    return;
  }
  uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, size = ctx.r5.u32;
  if (lr_forward_overlap(dst, src, size, 0x82D1ED20, uint32_t(ctx.lr))) {
    __imp__sub_82D1ED20(ctx, base);
    return;
  }
  if (size) std::memmove(base + dst, base + src, size);
  ctx.r3.u64 = dst;
}

// ---------------------------------------------------------------------------------------
// sub_829C4D28 is where the game waits for the GPU command processor to read its ring up
// to a point (it spins in sub_829C9810). Time spent here is the game thread held up by the
// renderer; logged every 10 s as "LR game waits".
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_829C4D28);
REX_HOOK_RAW(sub_829C4D28) {
  static std::atomic<uint64_t> waits{0}, wait_us{0}, max_us{0}, next_ms{0};
  auto t0 = std::chrono::steady_clock::now();
  __imp__sub_829C4D28(ctx, base);
  auto t1 = std::chrono::steady_clock::now();
  uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  lr_wait_note(kLrRenderer, 0x829C4D28, uint32_t(ctx.lr), us);
  if (us < 20) return;  // returned at once: no wait
  waits.fetch_add(1, std::memory_order_relaxed);
  wait_us.fetch_add(us, std::memory_order_relaxed);
  uint64_t m = max_us.load(std::memory_order_relaxed);
  while (us > m && !max_us.compare_exchange_weak(m, us)) {}
  uint64_t now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(t1.time_since_epoch()).count());
  uint64_t due = next_ms.load();
  if (now_ms >= due && next_ms.compare_exchange_strong(due, now_ms + 10000) && due) {
    REXLOG_INFO("LR game waits (10 s): game thread waited {} ms for the renderer in {} waits (longest {} ms)",
                wait_us.exchange(0) / 1000, waits.exchange(0), max_us.exchange(0) / 1000);
  }
}

// Settings menu items (Voice Language, Button Prompts).
#include "lr_config_menu.inc"
#include "lr_keyboard_icons.inc"
#include "lr_keyboard_menu.inc"

// ---------------------------------------------------------------------------------------
// Content packages the game opens (saves and DLC): one log line each, so a play log shows
// whether every installed DLC was found and licensed. sub_82D32278 is the game's
// XamContentCreateEx wrapper (r3 user, r4 root name, r5 XCONTENT_DATA, r6 flags,
// r7 disposition*, r8 license mask*); XCONTENT_DATA: +4 content type, +264 file name[42].
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_82D32278);
REX_HOOK_RAW(sub_82D32278) {
  char root[16] = {}, file[43] = {};
  for (int i = 0; i < 15 && ctx.r4.u32; ++i) {
    root[i] = char(base[ctx.r4.u32 + i]);
    if (!root[i]) break;
  }
  uint32_t data = ctx.r5.u32;
  uint32_t type = data ? lr_load_be32(base, data + 4) : 0;
  for (int i = 0; i < 42 && data; ++i) {
    file[i] = char(base[data + 264 + i]);
    if (!file[i]) break;
  }
  uint32_t license_ptr = ctx.r8.u32;
  __imp__sub_82D32278(ctx, base);
  static std::atomic<uint32_t> logged{0}, saves_logged{0};
  if (type == 2 && logged.fetch_add(1) < 64) {
    REXLOG_INFO("LR DLC: opened '{}' as {}: result {:08X}, license {:08X}", file, root, ctx.r3.u32,
                license_ptr ? lr_load_be32(base, license_ptr) : 0u);
  } else if (type == 1 && saves_logged.fetch_add(1) < 64) {  // saves: Load Game problems
    REXLOG_INFO("LR saves: opened '{}' as {}: result {:08X}", file, root, ctx.r3.u32);
  }
}

// ---------------------------------------------------------------------------------------
// DLC availability, one log line each:
//  - sub_82651678 adds a pack the DLC device found (name = the pack's identifier file; the
//    device keeps "DLC:/identifier" at +84 and the list at +448);
//  - sub_826438E0 lists a game directory; for "dlc/" the names are the pack numbers the game sees;
//  - sub_828F7920 builds the lists the shops read: found pack numbers (+18184, count +18440)
//    and the item ids they make available (+17924, count +18180), from the r_dlc table.
// ---------------------------------------------------------------------------------------
namespace {
std::string lr_guest_cstr(uint8_t* base, uint32_t p, size_t max) {
  std::string s;
  for (size_t i = 0; p && i < max && base[p + i]; ++i) {
    char c = char(base[p + i]);
    s.push_back((c >= 32 && c < 127) ? c : '.');
  }
  return s;
}
}  // namespace

REX_EXTERN(__imp__sub_82651678);
REX_HOOK_RAW(sub_82651678) {
  uint32_t entry = ctx.r4.u32 ? lr_load_be32(base, ctx.r4.u32) : 0;
  std::string name = entry ? lr_guest_cstr(base, entry, 32) : std::string();
  if (entry && name.size() == 7 && name.find_first_not_of("0123456789") == std::string::npos &&
      ctx.r3.u32 >= 448 && lr_guest_cstr(base, ctx.r3.u32 - 448 + 84, 16) == "DLC:/identifier") {
    REXLOG_INFO("LR DLC: pack '{}' found (identifier extra '{}')", lr_guest_cstr(base, entry, 32),
                lr_guest_cstr(base, entry + 324, 16));
  }
  __imp__sub_82651678(ctx, base);
}

REX_EXTERN(__imp__sub_826438E0);
REX_HOOK_RAW(sub_826438E0) {
  std::string dir = lr_guest_cstr(base, ctx.r3.u32, 64);
  uint32_t out = ctx.r4.u32;
  __imp__sub_826438E0(ctx, base);
  if (dir != "dlc/" && dir != "img/dlc/") return;
  int32_t n = int32_t(ctx.r3.u32);
  std::string names;
  for (int32_t i = 0; i < n && i < 64; ++i) {
    if (i) names += ", ";
    names += lr_guest_cstr(base, out + uint32_t(i) * 280, 32);
  }
  REXLOG_INFO("LR DLC: directory {} lists {} entries: {}", dir, n, names);
}

// sub_828F6000 runs when a pack check finishes: the available-pack list the shops read
// (sub_828F55C0, sub_828F5AD0: 0x8312E8D8, count at +256) is set by then, and it rebuilds the
// names of items held back because their pack is missing (count at 0x8312E9F0, 16-byte names
// from 0x8312EA20).
// The held-back ids also go to dlc_unavailable.txt, and imported Steam and PS3 saves lose those
// items (lr_steam_saves::CleanImported) before one is loaded: this runs at boot, after the pack check.
REX_EXTERN(__imp__sub_828F6000);
REX_HOOK_RAW(sub_828F6000) {
  __imp__sub_828F6000(ctx, base);
  uint32_t n = std::min<uint32_t>(lr_load_be32(base, 0x8312E9F0), 128);
  std::string names;
  std::set<std::string> held_back;
  for (uint32_t i = 0; i < n; ++i) {
    std::string id = lr_guest_cstr(base, 0x8312EA20 + i * 16, 16);
    names += (i ? "," : "") + id;
    if (!id.empty()) held_back.insert(id);
  }
  if (!g_lr_settings_dir.empty()) {
    std::ofstream out(g_lr_settings_dir / "dlc_unavailable.txt", std::ios::trunc);
    out << "# DLC items the 360 game does not have (pack not installed), as it listed them; imported Steam\n"
           "# saves lose these items (see lr_steam_saves.h)\n";
    for (const auto& id : held_back) out << id << '\n';
    out.close();
    lr_steam_saves::CleanImported(g_lr_settings_dir, held_back);
  }
  uint32_t shop_n = std::min<uint32_t>(lr_load_be32(base, 0x8312E9D8), 64);
  std::string packs;
  for (uint32_t i = 0; i < shop_n; ++i) packs += (i ? "," : "") + std::to_string(lr_load_be32(base, 0x8312E8D8 + i * 4));
  REXLOG_INFO("LR DLC: available packs [{}]; {} items held back (pack not installed): {}", packs, n, names);
}

// sub_8233F610 fills the Rewards Barter Shop's "Downloaded Items" pack list (r3 = list:
// pack numbers, count at +256) from the available-pack list.
REX_EXTERN(__imp__sub_8233F610);
REX_HOOK_RAW(sub_8233F610) {
  uint32_t list = ctx.r3.u32;
  __imp__sub_8233F610(ctx, base);
  uint32_t n = std::min<uint32_t>(lr_load_be32(base, list + 256), 64);
  std::string packs;
  for (uint32_t i = 0; i < n; ++i) packs += (i ? "," : "") + std::to_string(lr_load_be32(base, list + i * 4));
  REXLOG_INFO("LR DLC: shop lists {} packs [{}]", n, packs);
}

// sub_822EA910 mounts a newly found pack's archives (r4 pack number, r5 0 = img, 1 = scr).
REX_EXTERN(__imp__sub_822EA910);
REX_HOOK_RAW(sub_822EA910) {
  uint32_t id = ctx.r4.u32, part = ctx.r5.u32;
  __imp__sub_822EA910(ctx, base);
  REXLOG_INFO("LR DLC: pack {} {} archive mounted", id, part ? "scr" : "img");
}

// sub_82915120 runs on save data (r3): for every available pack without a date in the save
// (+0x11300, 64 words), it stores today's date there.
REX_EXTERN(__imp__sub_82915120);
REX_HOOK_RAW(sub_82915120) {
  uint32_t save = ctx.r3.u32;
  std::string before;
  for (uint32_t i = 0; i < 64; ++i)
    if (lr_load_be32(base, save + 0x11300 + i * 4)) before += (before.empty() ? "" : ",") + std::to_string(i);
  __imp__sub_82915120(ctx, base);
  std::string after;
  for (uint32_t i = 0; i < 64; ++i)
    if (lr_load_be32(base, save + 0x11300 + i * 4)) after += (after.empty() ? "" : ",") + std::to_string(i);
  REXLOG_INFO("LR DLC: save records packs [{}] (before this check [{}])", after, before);
}

REX_EXTERN(__imp__sub_828F7920);
REX_EXTERN(__imp__sub_82643C58);
REX_HOOK_RAW(sub_828F7920) {
  uint32_t job = ctx.r3.u32;
  __imp__sub_828F7920(ctx, base);
  uint32_t found = std::min<uint32_t>(lr_load_be32(base, job + 18440), 64);
  uint32_t avail = std::min<uint32_t>(lr_load_be32(base, job + 18180), 64);
  std::string a, b;
  for (uint32_t i = 0; i < found; ++i) a += (i ? "," : "") + std::to_string(lr_load_be32(base, job + 18184 + i * 4));
  for (uint32_t i = 0; i < avail; ++i) b += (i ? "," : "") + std::to_string(lr_load_be32(base, job + 17924 + i * 4));
  REXLOG_INFO("LR DLC: packs found [{}], items made available ({}) [{}]", a, avail, b);
}
