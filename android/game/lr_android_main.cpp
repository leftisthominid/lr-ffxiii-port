// Lightning Returns - Android entry point.
//
// SDL3's Java activity (org.libsdl.app.SDLActivity, subclassed by
// com.lightningreturns.port.LrActivity) loads librexruntime.so (which carries SDL3 and its JNI
// entry points) and libmain.so (this), then calls SDL_main on its SDL thread with the
// arguments from LrActivity.getArguments(): --game_data_root=..., --user_data_root=...,
// --log_file=... (see android/app/src/main/java/.../LrActivity.java).
//
// Replaces the SDK's windowed_app_main_sdl.cpp (main()) for this build.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#include "lr_android.h"

namespace {

constexpr const char* kTag = "LightningReturns";
// Logged into the game log once logging is up (the options are applied before it starts).
std::vector<std::string> g_lr_android_option_log;

int RunWindowedApp(int argc, char** argv) {
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();
  for (const auto& m : g_lr_android_option_log) REXLOG_INFO("{}", m);

  int result;
  {
    rex::ui::SDLWindowedAppContext app_context;
    if (!app_context.Initialize()) {
      __android_log_print(ANDROID_LOG_ERROR, kTag, "SDL windowed app context failed to initialize");
      return EXIT_FAILURE;
    }
    auto creator = rex::ui::WindowedApp::GetCreator("lightningreturns");
    if (!creator) {
      __android_log_print(ANDROID_LOG_ERROR, kTag, "app 'lightningreturns' is not registered");
      return EXIT_FAILURE;
    }
    std::unique_ptr<rex::ui::WindowedApp> app = creator(app_context);

    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;
    app->InvokeOnDestroy();
  }
  return result;
}

// Renderer settings for mobile GPUs. The GPU plugin registers its settings after the config
// file was read and takes them from the environment (REX_<NAME>), so they are set here, before
// the plugin loads:
//  - vulkan_sparse_shared_memory off: the 512 MB buffer that stands in for the 360's memory is
//    a plain buffer, not sparse (sparse binding on mobile drivers drops GPU writes, which turns
//    every copied render target - the 3D scene - black);
//  - present_sgsr + present_max_height 1080: the picture goes out at 1080 lines (2340x1080 on a
//    3120x1440 screen), which the display hardware stretches to the screen, and the game's 720p
//    frame is brought to it with Snapdragon Game Super Resolution 1 (Qualcomm; edge-aware
//    upscale and sharpening in one pass) instead of bilinear filtering. "present_sgsr = false" +
//    "present_max_height = 0" is how it was before;
//  - android_options.txt in the user data folder: "name = value" lines override these and set
//    any other renderer setting (a commented template is written on first start).
// Each value in effect is logged ("LR android option: ...").
constexpr const char* kLrAndroidOptionsTemplate =
    "# Lightning Returns (Android) - renderer options, one \"name = value\" per line.\n"
    "# Remove the # in front of a line to use it; the game reads this file at start.\n"
    "# Defaults here: vulkan_sparse_shared_memory = false, present_sgsr = true,\n"
    "# present_max_height = 1080\n"
    "#\n"
    "# present_sgsr = false\n"
    "# present_max_height = 0\n"
    "# vulkan_sparse_shared_memory = true\n"
    "# direct_host_resolve = false\n"
    "# vulkan_readback_resolve = true\n"
    "# async_shader_compilation = false\n"
    "# vulkan_dynamic_rendering = false\n"
    "# gamma_render_target_as_unorm16 = false\n"
    "# snorm16_render_target_full_range = false\n"
    "# lr_frame_capture = true\n";

std::string Trim(std::string v) {
  auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!v.empty() && ws(v.back())) v.pop_back();
  size_t i = 0;
  while (i < v.size() && ws(v[i])) ++i;
  return v.substr(i);
}

void ApplyAndroidRendererOptions(const std::string& user_root) {
  std::map<std::string, std::string> options = {{"vulkan_sparse_shared_memory", "false"},
                                                 {"present_sgsr", "true"},
                                                 {"present_max_height", "1080"}};
  std::map<std::string, std::string> from_file;
  if (!user_root.empty()) {
    std::filesystem::path file = std::filesystem::path(user_root) / "android_options.txt";
    if (!std::filesystem::exists(file)) {
      if (FILE* f = std::fopen(file.c_str(), "w")) {
        std::fputs(kLrAndroidOptionsTemplate, f);
        std::fclose(f);
      }
    } else if (FILE* f = std::fopen(file.c_str(), "r")) {
      char line[512];
      while (std::fgets(line, sizeof line, f)) {
        std::string l = Trim(line);
        if (l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string name = Trim(l.substr(0, eq)), value = Trim(l.substr(eq + 1));
        if (name.empty()) continue;
        options[name] = value;
        from_file[name] = value;
      }
      std::fclose(f);
    }
  }
  for (const auto& [name, value] : options) {
    std::string env = "REX_";
    for (char c : name) env += char(std::toupper(static_cast<unsigned char>(c)));
    setenv(env.c_str(), value.c_str(), 1);
    std::string msg = "LR android option: " + name + " = " + value +
                      (from_file.count(name) ? " (android_options.txt)" : " (default)");
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", msg.c_str());
    g_lr_android_option_log.push_back(msg);
  }
  // Which APK this is (LrActivity sets it from the package's versionName).
  if (const char* v = std::getenv("LR_APK_VERSION")) {
    g_lr_android_option_log.insert(g_lr_android_option_log.begin(), std::string("LR android build: ") + v);
  }
  // The game log starts after this; the game logs these lines at startup.
  std::string joined;
  for (const auto& m : g_lr_android_option_log) joined += m + "\n";
  setenv("LR_ANDROID_OPTIONS_LOG", joined.c_str(), 1);
}

std::string FlagValue(const std::vector<std::string>& args, const char* flag) {
  std::string prefix = std::string(flag) + "=";
  for (const auto& a : args) {
    if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
  }
  return {};
}

bool HasFlag(const std::vector<std::string>& args, const char* flag) {
  std::string prefix = std::string(flag) + "=";
  for (const auto& a : args) {
    if (a == flag || a.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

// The guest memory layout needs 4 KB host pages (the 0xE0000000 physical heap is mapped at a
// 4 KB offset; on Linux the runtime is built without the >4 KB-granularity compensation).
bool CheckPageSize() {
  long page = sysconf(_SC_PAGESIZE);
  if (page == 4096) return true;
  std::string message = "This device uses " + std::to_string(page / 1024) +
                        " KB memory pages. The game needs a device (or kernel) with 4 KB pages.";
  __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", message.c_str());
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Lightning Returns", message.c_str(), nullptr);
  return false;
}

}  // namespace

// Android performance hint (ADPF, API 33+, looked up at run time; minSdk is 29). Android sizes
// CPU clocks by load, and this game's frame is many short hand-offs between threads (hundreds of
// waits per frame), which reads as light load: clocks stay low and thread wake-ups are slow.
// Reporting each frame's work against the frame target lets the system raise clocks and keep
// the threads on fast cores when frames run long.
namespace {
struct LrHint {
  using GetManager = void* (*)();
  using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
  using UpdateTarget = int (*)(void*, int64_t);
  using ReportActual = int (*)(void*, int64_t);
  using CloseSession = void (*)(void*);
  using SetThreads = int (*)(void*, const pid_t*, size_t);
  using SetPreferPowerEfficiency = int (*)(void*, bool);
  GetManager get_manager = nullptr;
  CreateSession create = nullptr;
  UpdateTarget update_target = nullptr;
  ReportActual report = nullptr;
  CloseSession close = nullptr;
  SetThreads set_threads = nullptr;
  SetPreferPowerEfficiency prefer_power_efficiency = nullptr;
  void* manager = nullptr;
  void* session = nullptr;
  bool tried = false;
  int64_t target_ns = 0;
  uint64_t frames = 0;
  std::vector<pid_t> threads;
  std::map<pid_t, uint64_t> last_cpu;  // per thread, CPU ticks at the last selection
};
LrHint g_lr_hint;

std::string LrThreadName(pid_t tid) {
  char path[64];
  std::snprintf(path, sizeof path, "/proc/self/task/%d/comm", int(tid));
  std::string name;
  if (FILE* f = std::fopen(path, "r")) {
    char buf[64] = {};
    if (std::fgets(buf, sizeof buf, f)) name = buf;
    std::fclose(f);
  }
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
  return name;
}

uint64_t LrThreadCpuTicks(pid_t tid) {
  char path[64];
  std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", int(tid));
  uint64_t ticks = 0;
  if (FILE* f = std::fopen(path, "r")) {
    char buf[1024] = {};
    size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    // Fields after the ")" of the name: state is field 3; utime and stime are fields 14, 15.
    if (const char* p = std::strrchr(buf, ')')) {
      unsigned long long ut = 0, st = 0;
      if (std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2)
        ticks = ut + st;
    }
  }
  return ticks;
}

// The main thread and the busiest threads since the last pick (the GPU command thread, the game's
// job workers, audio): by CPU time, not name - most guest threads carry no name on Android.
std::vector<pid_t> LrPickHintThreads(pid_t main_tid) {
  std::vector<pid_t> picked{main_tid};
  std::vector<std::pair<uint64_t, pid_t>> busy;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator("/proc/self/task", ec)) {
    pid_t tid = pid_t(std::atoi(e.path().filename().c_str()));
    if (tid <= 0 || tid == main_tid) continue;
    std::string name = LrThreadName(tid);
    uint64_t ticks = LrThreadCpuTicks(tid);
    uint64_t delta = ticks - std::min(ticks, g_lr_hint.last_cpu[tid]);
    g_lr_hint.last_cpu[tid] = ticks;
    // Skip the pipeline creation and shader threads: background work, not the frame.
    if (name.rfind("Vulkan Pipeline", 0) == 0 || name.rfind("Vulkan Storage", 0) == 0) continue;
    if (delta) busy.emplace_back(delta, tid);
  }
  std::sort(busy.rbegin(), busy.rend());
  for (size_t i = 0; i < busy.size() && i < 10; ++i) picked.push_back(busy[i].second);
  std::sort(picked.begin(), picked.end());
  return picked;
}
}  // namespace

namespace {
// Frame statistics for the performance readout (LrTouch.java): the game's frames over the last
// half second.
std::mutex g_lr_stats_mutex;
float g_lr_stats[3] = {0, 0, 0};  // fps, average frame ms, slowest frame ms
std::atomic<uint64_t> g_lr_game_frames{0};  // the game's frames so far (the loading text waits for one)
constexpr int kLrFrameRing = 120;
float g_lr_frame_ring[kLrFrameRing] = {};  // the last frames' times (ms), for the graph
int g_lr_frame_ring_pos = 0;
}  // namespace

void LrAndroidFrameDone(uint64_t work_us, double frame_rate) {
  g_lr_game_frames.fetch_add(1, std::memory_order_relaxed);
  {
    static auto window_start = std::chrono::steady_clock::now();
    static auto last = window_start;
    static uint32_t frames = 0;
    static double slowest_ms = 0;
    auto now = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    {
      std::lock_guard<std::mutex> lock(g_lr_stats_mutex);
      g_lr_frame_ring[g_lr_frame_ring_pos] = float(ms);
      g_lr_frame_ring_pos = (g_lr_frame_ring_pos + 1) % kLrFrameRing;
    }
    ++frames;
    slowest_ms = std::max(slowest_ms, ms);
    double window_ms = std::chrono::duration<double, std::milli>(now - window_start).count();
    if (window_ms >= 500) {
      std::lock_guard<std::mutex> lock(g_lr_stats_mutex);
      g_lr_stats[0] = float(frames * 1000.0 / window_ms);
      g_lr_stats[1] = float(window_ms / frames);
      g_lr_stats[2] = float(slowest_ms);
      frames = 0;
      slowest_ms = 0;
      window_start = now;
    }
  }
  LrHint& h = g_lr_hint;
  if (!h.tried) {
    h.tried = true;
    if (void* lib = dlopen("libandroid.so", RTLD_NOW)) {
      h.get_manager = reinterpret_cast<LrHint::GetManager>(dlsym(lib, "APerformanceHint_getManager"));
      h.create = reinterpret_cast<LrHint::CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
      h.update_target = reinterpret_cast<LrHint::UpdateTarget>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
      h.report = reinterpret_cast<LrHint::ReportActual>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
      h.close = reinterpret_cast<LrHint::CloseSession>(dlsym(lib, "APerformanceHint_closeSession"));
      h.set_threads = reinterpret_cast<LrHint::SetThreads>(dlsym(lib, "APerformanceHint_setThreads"));
      h.prefer_power_efficiency = reinterpret_cast<LrHint::SetPreferPowerEfficiency>(
          dlsym(lib, "APerformanceHint_setPreferPowerEfficiency"));
    }
    if (h.get_manager && h.create && h.report && h.update_target) h.manager = h.get_manager();
    if (!h.manager) REXLOG_INFO("LR performance hint: not available on this Android version");
  }
  if (!h.manager) return;
  ++h.frames;
  int64_t target_ns = int64_t(1e9 / std::clamp(frame_rate, 10.0, 120.0));
  // Pick the threads after the game has started its worker threads, then again every 10 s or so
  // (the busiest game threads change between the field, battles and scenes).
  if (!h.session ? h.frames >= 150 : (h.frames % 300) == 0) {
    std::vector<pid_t> threads = LrPickHintThreads(pid_t(gettid()));
    if (threads != h.threads) {
      if (h.session && h.set_threads && h.set_threads(h.session, threads.data(), threads.size()) == 0) {
        h.threads = threads;
      } else {
        if (h.session && h.close) h.close(h.session);
        std::vector<int32_t> ids(threads.begin(), threads.end());
        h.session = h.create(h.manager, ids.data(), ids.size(), target_ns);
        h.target_ns = target_ns;
        h.threads = threads;
        if (h.session && h.prefer_power_efficiency) h.prefer_power_efficiency(h.session, false);
      }
      std::string names;
      for (pid_t t : h.threads) names += " " + LrThreadName(t);
      REXLOG_INFO("LR performance hint: {} ({} threads:{}), target {:.1f} ms",
                  h.session ? "on" : "could not start", h.threads.size(), names, target_ns / 1e6);
    }
  }
  if (!h.session) return;
  if (target_ns != h.target_ns) {
    h.update_target(h.session, target_ns);
    h.target_ns = target_ns;
  }
  if (work_us) h.report(h.session, int64_t(work_us) * 1000);
}


// JNI for com.lightningreturns.port.LrTouch (the touch layer over the game).
int LrConfigGetUiStyle();  // lr_hooks.cpp: 1 when the game shows PS3 prompts
extern std::atomic<bool> g_lr_screenshot_request;  // lr_hooks.cpp

extern "C" {
JNIEXPORT void JNICALL Java_com_lightningreturns_port_LrTouch_nativeSetPad(
    JNIEnv*, jclass, jboolean active, jint buttons, jint lt, jint rt, jint lx, jint ly, jint rx, jint ry) {
  auto axis = [](jint v) { return int16_t(std::clamp<jint>(v, -32768, 32767)); };
  LrTouchSetPad(active, uint16_t(buttons), uint8_t(std::clamp<jint>(lt, 0, 255)),
                uint8_t(std::clamp<jint>(rt, 0, 255)), axis(lx), axis(ly), axis(rx), axis(ry));
}
JNIEXPORT void JNICALL Java_com_lightningreturns_port_LrTouch_nativeMenuOpen(JNIEnv*, jclass, jboolean open) {
  LrTouchSetMenuOpen(open);
}
JNIEXPORT jint JNICALL Java_com_lightningreturns_port_LrTouch_nativeMenuButtons(JNIEnv*, jclass) {
  return jint(LrTouchTakeMenuButtons());
}
JNIEXPORT jboolean JNICALL Java_com_lightningreturns_port_LrTouch_nativePhysicalActivity(JNIEnv*, jclass) {
  return LrTouchTakePhysicalActivity();
}
JNIEXPORT jfloatArray JNICALL Java_com_lightningreturns_port_LrTouch_nativeStats(JNIEnv* env, jclass) {
  float v[3];
  {
    std::lock_guard<std::mutex> lock(g_lr_stats_mutex);
    std::memcpy(v, g_lr_stats, sizeof v);
  }
  jfloatArray out = env->NewFloatArray(3);
  if (out) env->SetFloatArrayRegion(out, 0, 3, v);
  return out;
}
// The last 120 frame times (ms), oldest first.
JNIEXPORT jfloatArray JNICALL Java_com_lightningreturns_port_LrTouch_nativeFrameTimes(JNIEnv* env, jclass) {
  float v[kLrFrameRing];
  {
    std::lock_guard<std::mutex> lock(g_lr_stats_mutex);
    for (int i = 0; i < kLrFrameRing; ++i) v[i] = g_lr_frame_ring[(g_lr_frame_ring_pos + i) % kLrFrameRing];
  }
  jfloatArray out = env->NewFloatArray(kLrFrameRing);
  if (out) env->SetFloatArrayRegion(out, 0, kLrFrameRing, v);
  return out;
}
JNIEXPORT jlong JNICALL Java_com_lightningreturns_port_LrTouch_nativeGameFrames(JNIEnv*, jclass) {
  return jlong(g_lr_game_frames.load(std::memory_order_relaxed));
}
JNIEXPORT jboolean JNICALL Java_com_lightningreturns_port_LrTouch_nativePs3Prompts(JNIEnv*, jclass) {
  return LrConfigGetUiStyle() != 0;
}
JNIEXPORT void JNICALL Java_com_lightningreturns_port_LrTouch_nativeScreenshot(JNIEnv*, jclass) {
  g_lr_screenshot_request.store(true);
}
}  // extern "C"

// Asks LrActivity to relaunch the app in a new process (see LrActivity.lrRestartApp()).
bool LrAndroidRestartProcess(int generation) {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return false;
  bool ok = false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID method = cls ? env->GetMethodID(cls, "lrRestartApp", "(I)Z") : nullptr;
  if (method) {
    ok = env->CallBooleanMethod(activity, method, jint(generation)) == JNI_TRUE;
  }
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    ok = false;
  }
  if (cls) env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  __android_log_print(ANDROID_LOG_INFO, kTag, "restart (generation %d) %s", generation,
                      ok ? "scheduled" : "failed");
  return ok;
}

int main(int argc, char* argv[]) {
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");
  if (args.empty()) args.emplace_back("lightningreturns");

  if (!CheckPageSize()) return EXIT_FAILURE;

  std::string game_root = FlagValue(args, "--game_data_root");
  std::string user_root = FlagValue(args, "--user_data_root");
  if (game_root.empty() || !std::filesystem::is_directory(game_root)) {
    std::string message = "Game data folder not found:\n" +
                          (game_root.empty() ? std::string("(none chosen)") : game_root) +
                          "\n\nCopy the game files there (see README) and start again.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Lightning Returns", message.c_str(), nullptr);
    return EXIT_FAILURE;
  }
  // Config file (lightningreturns.toml) and default log folder: the user data folder.
  if (!user_root.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(user_root, ec);
    setenv("REX_ANDROID_APP_DIR", user_root.c_str(), 1);
  }
  ApplyAndroidRendererOptions(user_root);
  // Texture replacement root (PS3 button / UI set, sharp fonts, texture mods): the PC launcher
  // sets it to <install>\textures; here <user data>/textures, which the install script fills from
  // the PC (LR_Android_Install.ps1). Without it the root was "/textures" and nothing was found.
  if (!user_root.empty()) {
    std::string textures = (std::filesystem::path(user_root) / "textures").string();
    setenv("LR_TEXTURES", textures.c_str(), 0);
  }
  // Frame captures for renderer diagnostics (with "lr_frame_capture = true" in
  // android_options.txt, each screenshot also records the frame, or a file named "request" in
  // the captures folder): see the GPU plugin, command_processor.cpp. Off by default: a capture
  // stalls the game for seconds.
  const char* capture_option = std::getenv("REX_LR_FRAME_CAPTURE");
  if (!user_root.empty() && capture_option && std::string(capture_option) == "true") {
    std::string captures = (std::filesystem::path(user_root) / "captures").string();
    std::error_code ec;
    std::filesystem::create_directories(captures, ec);
    setenv("LR_CAPTURE_DIR", captures.c_str(), 1);
  }
  // Trace of the battle-exit grey filter ("LR fx" lines; see lr_frame_stats.h), off since the fix
  // (vulkan_lr_full_barriers): "lr_fx_trace = true" in android_options.txt turns it on.
  const char* fx_option = std::getenv("REX_LR_FX_TRACE");
  setenv("LR_FX_TRACE", fx_option && std::string(fx_option) == "true" ? "1" : "0", 1);
  // Japanese voices and text (Settings > Voice Language / Text Language): the PC's jpvoice folder,
  // which LR_Android_Install.ps1 copies to <user data>/jpvoice - as the PC exe finds it next to
  // itself. Without it both settings stay locked to English.
  if (!user_root.empty() && !HasFlag(args, "--update_data_root")) {
    std::filesystem::path jp = std::filesystem::path(user_root) / "jpvoice";
    std::error_code ec;
    if (std::filesystem::exists(jp / "filelist_patch_u.x360.bin", ec)) {
      args.emplace_back("--update_data_root=" + jp.string());
      setenv("LR_JP_TEXT_MAP", (jp / "jp_text.txt").c_str(), 0);
      setenv("LR_JP_LIPSYNC_LIST", (jp / "jp_lipsync.txt").c_str(), 0);
      __android_log_print(ANDROID_LOG_INFO, kTag, "Japanese voices / text: %s", jp.c_str());
    } else {
      __android_log_print(ANDROID_LOG_INFO, kTag, "Japanese voices / text: not installed (%s)", jp.c_str());
    }
  }
  if (!HasFlag(args, "--gpu_plugin")) args.emplace_back("--gpu_plugin=xenos");
  if (!HasFlag(args, "--graphics_backend")) args.emplace_back("--graphics_backend=vulkan");

  std::string joined;
  for (const auto& a : args) joined += " " + a;
  __android_log_print(ANDROID_LOG_INFO, kTag, "SDL_main:%s", joined.c_str());

  std::vector<char*> argv_ptrs;
  for (auto& a : args) argv_ptrs.push_back(a.data());
  argv_ptrs.push_back(nullptr);
  int result = RunWindowedApp(int(args.size()), argv_ptrs.data());
  __android_log_print(ANDROID_LOG_INFO, kTag, "game exited (%d)", result);
  rex::FlushLogging();
  // The runtime's guest threads are not torn down cleanly; end the process like the PC build.
  _exit(result);
}
