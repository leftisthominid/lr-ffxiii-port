// lightningreturns - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/audio/audio_driver.h>
#include <rex/cvar.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <imgui.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window_listener.h>

#include <SDL3/SDL.h>

// PNG decoding for the Gamepad Controls picture (static: no clash with other copies of stb_image).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "lr_steam_saves.h"
#include "lr_ps3_saves.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Share button through Microsoft's GameInput (API 3, lr_gameinput.h: Microsoft's MIT-licensed
// header as SDL ships it). Its system-button callback is the only place Windows reports Share
// for USB / wireless-adapter Xbox controllers. The runtime is GameInputRedist.dll (the GameInput
// redistributable, which LR_Step53.ps1 installs) or a recent inbox GameInput.dll; without one,
// SDL alone reads the controller.
#if defined(_WIN32) && defined(__has_include)
#if __has_include("lr_gameinput.h")
#include "lr_gameinput.h"
#if defined(GAMEINPUT_API_VERSION) && GAMEINPUT_API_VERSION >= 1
#define LR_GAMEINPUT_SHARE 1
#include <atomic>
#include <cstdint>
#include <string>
#define LR_GI_CAT2(a, b) a##b
#define LR_GI_CAT(a, b) LR_GI_CAT2(a, b)
namespace lr_gameinput {
using namespace GameInput::LR_GI_CAT(v, GAMEINPUT_API_VERSION);
inline std::atomic<bool> g_share_down{false};
inline void CALLBACK OnSystemButtons(GameInputCallbackToken, void*, IGameInputDevice*, uint64_t,
                                     GameInputSystemButtons current, GameInputSystemButtons) {
  g_share_down.store((current & GameInputSystemButtonShare) != 0, std::memory_order_relaxed);
}
// The runtime, as Microsoft's loader looks for it: GameInputRedist.dll in System32 or in the
// redistributable's folder (HKLM\SOFTWARE\Microsoft\GameInput RedistDir), else GameInput.dll.
inline HMODULE LoadRuntime() {
  wchar_t sys[MAX_PATH];
  UINT n = GetSystemDirectoryW(sys, MAX_PATH);
  std::wstring system32 = n ? std::wstring(sys, n) + L"\\" : std::wstring();
  if (HMODULE m = LoadLibraryW((system32 + L"GameInputRedist.dll").c_str())) return m;
  wchar_t dir[MAX_PATH];
  DWORD size = sizeof(dir);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\GameInput", L"RedistDir",
                   RRF_RT_REG_SZ | 0x00020000 /* RRF_SUBKEY_WOW6432KEY */, nullptr, dir, &size) == ERROR_SUCCESS) {
    std::wstring path(dir);
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    if (HMODULE m = LoadLibraryW((path + L"GameInputRedist.dll").c_str())) return m;
  }
  return LoadLibraryW((system32 + L"GameInput.dll").c_str());
}
// Registers the callback. Returns what went wrong, for the log (nullptr: working).
inline const char* Start() {
  HMODULE dll = LoadRuntime();
  if (!dll) return "no GameInput runtime found (install the GameInput redistributable)";
  using Init = HRESULT(WINAPI*)(REFIID, void**);
  auto init = reinterpret_cast<Init>(reinterpret_cast<void*>(GetProcAddress(dll, "GameInputInitialize")));
  if (!init) return "the GameInput runtime is too old (no GameInputInitialize; install the redistributable)";
  IGameInput* gi = nullptr;
  if (FAILED(init(IID_IGameInput, reinterpret_cast<void**>(&gi))) || !gi)
    return "the GameInput runtime does not offer API 3 (install the current redistributable)";
  GameInputCallbackToken token = 0;
  if (FAILED(gi->RegisterSystemButtonCallback(nullptr, GameInputSystemButtonShare, nullptr, OnSystemButtons, &token)))
    return "RegisterSystemButtonCallback failed";
  return nullptr;  // gi stays alive for the callback
}
}  // namespace lr_gameinput
#endif
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern std::atomic<bool> g_lr_keyboard_mode;  // lr_hooks.cpp: keystrokes made from the pad state
extern std::atomic<bool> g_lr_screenshot_request;  // lr_hooks.cpp: take a screenshot now (Android button)
extern std::atomic<int> g_lr_controls_open;   // lr_keyboard_menu.inc: Settings > Gamepad Controls
extern std::function<void(bool)> g_lr_controls_ui;  // shows/hides the picture (set by the app)
std::filesystem::path LrControlsPicture();
void LrKeyInput(rex::ui::VirtualKey key, bool down, bool repeat);  // lr_keyboard_menu.inc

// The game window's key events for Settings > Keyboard Settings (it waits for the key to assign).
// Runs first and never consumes an event.
class LrKeyListener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override { LrKeyInput(e.virtual_key(), true, e.prev_state()); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { LrKeyInput(e.virtual_key(), false, false); }
};

void LrSettingsInit(const std::filesystem::path& dir);
int LrRecentFrameTimesMs(float* out, int max);
void LrResolutionStartup(const std::filesystem::path& settings_dir);
void LrFrameRateStartup();     // lr_hooks.cpp: the 120 Hz vertical blank for 120 fps
void LrFrameRateAfterSetup();  // lr_hooks.cpp: the game is told 60 Hz again
int LrConfigGetUiStyle();
void LrConfigSetUiStyle(int ps3);
void LrUiStyleForController(int ps3, const char* name);

// With no sound output device (none plugged in, or all disabled) the runtime's audio driver
// fails to open, the game's XAudio registration fails, and the movie player crashes. This
// backend falls back to a silent driver that takes the game's audio frames at the real rate
// (256 samples at 48 kHz), so the game runs normally without sound.
class LrSilentAudioDriver : public rex::audio::AudioDriver {
 public:
  LrSilentAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore), thread_([this] { Run(); }) {}
  ~LrSilentAudioDriver() override {
    running_ = false;
    thread_.join();
  }
  void SubmitFrame(uint32_t) override { ++queued_; }

 private:
  void Run() {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const auto frame = std::chrono::microseconds(16000) / 3;  // 256 samples at 48 kHz
    uint64_t played = 0;
    while (running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      // Catches up after a long sleep (coarse timers): a frame plays every 5.33 ms, as on a
      // sound device, and the game is told when a frame it queued has played.
      for (uint64_t due = uint64_t((clock::now() - start) / frame); played < due; ++played) {
        int q = queued_.load();
        while (q > 0 && !queued_.compare_exchange_weak(q, q - 1)) {
        }
        if (q > 0) semaphore_->Release(1, nullptr);
      }
    }
  }
  rex::thread::Semaphore* semaphore_;
  std::atomic<int> queued_{0};
  std::atomic<bool> running_{true};
  std::thread thread_;
};

class LrAudioSystem : public rex::audio::sdl::SDLAudioSystem {
  using X_STATUS = rex::X_STATUS;

 public:
  using SDLAudioSystem::SDLAudioSystem;
  static std::unique_ptr<rex::audio::AudioSystem> Create(rex::runtime::FunctionDispatcher* dispatcher) {
    return std::make_unique<LrAudioSystem>(dispatcher);
  }
  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override {
    if (XSUCCEEDED(SDLAudioSystem::CreateDriver(index, semaphore, out_driver))) return X_STATUS_SUCCESS;
    REXLOG_WARN("LR audio: no sound output device could be opened; the game runs without sound "
                "(connect or enable one and restart the game for sound)");
    *out_driver = new LrSilentAudioDriver(memory(), semaphore);
    return X_STATUS_SUCCESS;
  }
  void DestroyDriver(rex::audio::AudioDriver* driver) override {
    if (auto* silent = dynamic_cast<LrSilentAudioDriver*>(driver)) {
      delete silent;
      return;
    }
    SDLAudioSystem::DestroyDriver(driver);
  }
};

// Settings > Gamepad Controls: the Steam version's controls picture (a 1024x480 PNG, from the step
// script) over a darkened game, at its 720p size scaled to the window. It exists only while the
// picture is up (an ImGui dialog makes the UI repaint every frame); the input hook closes it.
struct LrControlsPictureTexture {
  std::filesystem::path path;
  std::unique_ptr<rex::ui::ImmediateTexture> texture;  // kept for the next time it opens
};

class LrControlsOverlay final : public rex::ui::ImGuiDialog {
 public:
  LrControlsOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                    LrControlsPictureTexture* picture)
      : ImGuiDialog(drawer), immediate_(immediate), picture_(picture) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const int open = g_lr_controls_open.load(std::memory_order_relaxed);
    if (open != 1 && open != 2) return;
    std::filesystem::path path = LrControlsPicture();
    if (path != picture_->path) {
      picture_->path = path;
      picture_->texture.reset();
      std::ifstream in(path, std::ios::binary);
      std::vector<uint8_t> png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      int w = 0, h = 0, channels = 0;
      stbi_uc* rgba = png.empty() ? nullptr
                                  : stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &channels, 4);
      if (rgba && w == int(kWidth) && h == int(kHeight) && immediate_) {
        picture_->texture =
            immediate_->CreateTexture(kWidth, kHeight, rex::ui::ImmediateTextureFilter::kLinear, false, rgba);
      }
      if (rgba) stbi_image_free(rgba);
      REXLOG_INFO("LR controls: picture {} {}", path.string(),
                  picture_->texture ? "loaded" : "could not be loaded");
    }
    if (!picture_->texture) return;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const ImVec2 size = io.DisplaySize;
    list->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 200));
    const float s = std::min(size.x / 1280.0f, size.y / 720.0f);
    const ImVec2 p0((size.x - kWidth * s) / 2.0f, (size.y - kHeight * s) / 2.0f);
    list->AddImage(reinterpret_cast<ImTextureID>(picture_->texture.get()), p0,
                   ImVec2(p0.x + kWidth * s, p0.y + kHeight * s));
  }

 private:
  static constexpr uint32_t kWidth = 1024, kHeight = 480;
  rex::ui::ImmediateDrawer* immediate_;
  LrControlsPictureTexture* picture_;
};

// Frame rate and frame time graph in the top-right corner (F2, bind_lr_frame_overlay). The
// times are the game's own frames (its main loop, lr_hooks.cpp), not the window's redraws.
class LrFrameOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit LrFrameOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    constexpr int kMax = 240;
    float ms[kMax];
    int n = LrRecentFrameTimesMs(ms, kMax);
    if (n < 2) return;
    // Frame rate over the last half second; the average, slowest, and target lines below.
    double sum = 0;
    int count = 0;
    for (int i = n - 1; i >= 0 && sum < 500.0; --i, ++count) sum += ms[i];
    double fps = sum > 0 ? 1000.0 * count / sum : 0;
    float worst = 0;
    for (int i = 0; i < n; ++i) worst = std::max(worst, ms[i]);

    float s = std::max(1.0f, io.DisplaySize.y / 1080.0f);
    float pad = 12.0f * s;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, pad), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s, 6.0f * s));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##lr_frame_overlay", nullptr, flags)) {
      ImGui::SetWindowFontScale(s);
      ImU32 fps_color = fps >= 55 ? IM_COL32(120, 230, 120, 255)
                        : fps >= 28 ? IM_COL32(240, 220, 110, 255)
                                    : IM_COL32(250, 110, 100, 255);
      ImGui::PushStyleColor(ImGuiCol_Text, fps_color);
      ImGui::Text("%.0f FPS", fps);
      ImGui::PopStyleColor();
      ImGui::SameLine();
      ImGui::TextDisabled("%.1f ms  max %.0f ms", count ? sum / count : 0.0, worst);

      // Graph: one bar per frame, oldest left; lines at 16.7 ms (60 fps) and 33.3 ms (30 fps).
      ImVec2 size(240.0f * s, 56.0f * s);
      ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImGui::Dummy(size);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      float top = std::max(50.0f, worst * 1.1f);
      auto y_of = [&](float v) { return p0.y + size.y - std::min(v, top) / top * size.y; };
      dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(0, 0, 0, 90));
      for (float line : {16.7f, 33.3f}) {
        float y = y_of(line);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + size.x, y), IM_COL32(255, 255, 255, 60));
      }
      float w = size.x / kMax;
      int first = kMax - n;
      for (int i = 0; i < n; ++i) {
        float v = ms[i];
        ImU32 c = v <= 17.5f ? IM_COL32(120, 230, 120, 220)
                  : v <= 34.5f ? IM_COL32(240, 220, 110, 220)
                               : IM_COL32(250, 110, 100, 230);
        float x = p0.x + (first + i) * w;
        dl->AddRectFilled(ImVec2(x, y_of(v)), ImVec2(x + std::max(1.0f, w - 0.5f), p0.y + size.y), c);
      }
    }
    ImGui::End();
    ImGui::PopStyleVar();
  }
};

class LightningreturnsApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<LightningreturnsApp>(new LightningreturnsApp(ctx, "lightningreturns",
        PPCImageConfig));
  }

  // Setup tool mode: LR_EXTRACT=<360 disc image (.iso) or package (Games on Demand header
  // file, DLC, title update)> and LR_EXTRACT_TO=<folder> copy every file out of the image and
  // exit before any window opens. Exit code 0 = done.
  void OnPostInitLogging() override {
    const char* src = std::getenv("LR_EXTRACT");
    const char* dst = std::getenv("LR_EXTRACT_TO");
    if (!src || !*src || !dst || !*dst) return;
    int rc = ExtractImage(std::filesystem::u8path(src), std::filesystem::u8path(dst));
    rex::ShutdownLogging();
    std::fflush(stdout);
    std::_Exit(rc);
  }

  static int ExtractImage(const std::filesystem::path& src, const std::filesystem::path& dst) {
    char magic[4] = {};
    {
      std::ifstream f(src, std::ios::binary);
      if (!f) {
        REXLOG_ERROR("LR extract: cannot open {}", src.string());
        return 2;
      }
      f.read(magic, 4);
    }
    bool package = !std::memcmp(magic, "LIVE", 4) || !std::memcmp(magic, "PIRS", 4) ||
                   !std::memcmp(magic, "CON ", 4);
    std::unique_ptr<rex::filesystem::Device> device;
    if (package) {
      device = std::make_unique<rex::filesystem::StfsContainerDevice>("", src);
    } else {
      device = std::make_unique<rex::filesystem::DiscImageDevice>("", src);
    }
    if (!device->Initialize()) {
      REXLOG_ERROR("LR extract: {} is not a 360 {} the SDK can read", src.string(),
                   package ? "package" : "disc image");
      return 3;
    }
    REXLOG_INFO("LR extract: {} ({}) -> {}", src.string(), package ? "package" : "disc image",
                dst.string());
    std::vector<rex::filesystem::Entry*> todo{device->ResolvePath("/")};
    if (!todo[0]) return 3;
    std::vector<uint8_t> buffer(16u << 20);
    uint64_t total = 0, next_report = 0;
    size_t files = 0;
    while (!todo.empty()) {
      auto* entry = todo.back();
      todo.pop_back();
      for (auto& child : entry->children()) todo.push_back(child.get());
      if (entry->path().empty()) continue;
      std::string rel = entry->path();
      std::replace(rel.begin(), rel.end(), '\\', '/');
      while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
      auto out_path = dst / std::filesystem::u8path(rel);
      std::error_code ec;
      if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
        std::filesystem::create_directories(out_path, ec);
        continue;
      }
      std::filesystem::create_directories(out_path.parent_path(), ec);
      rex::filesystem::File* in = nullptr;
      if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &in) != rex::X_STATUS(0) || !in) {
        REXLOG_ERROR("LR extract: cannot open {} in the image", entry->path());
        return 4;
      }
      std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
      size_t done = 0, size = entry->size();
      while (done < size) {
        size_t want = std::min(buffer.size(), size - done), got = 0;
        if (in->ReadSync(std::span<uint8_t>(buffer.data(), want), done, &got) != rex::X_STATUS(0) ||
            got == 0) {
          break;
        }
        out.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(got));
        done += got;
        total += got;
        if (total >= next_report) {
          REXLOG_INFO("LR extract: {} MB", total >> 20);
          std::printf("  %llu MB\n", (unsigned long long)(total >> 20));
          std::fflush(stdout);
          next_report = total + (256ull << 20);
        }
      }
      in->Destroy();
      if (done != size || !out) {
        REXLOG_ERROR("LR extract: could not copy {} ({} of {} bytes)", entry->path(), done, size);
        return 5;
      }
      files++;
    }
    REXLOG_INFO("LR extract: done, {} files, {} MB", files, total >> 20);
    std::printf("  done: %zu files, %llu MB\n", files, (unsigned long long)(total >> 20));
    return 0;
  }

  // Boot runs sub_822F44B0, which calls vtable[0] of a freshly built object
  // (0x822F63A4 -> 0x823F2438 -> 0x828D8260 -> 0x828D74B8 -> 0x82597C28 ->
  // 0x82596E28) before it constructs the manager that stores the table pointer
  // at 0x830DDE10 (0x822F63B8 -> 0x824A29F4). sub_82596E28 unconditionally
  // loads two words from [0x830DDE10] + 0x18E64 at 0x82597850..0x82597894, so
  // with the pointer still null it reads guest 0x00018E64. The values are only
  // copied into the caller's object, so the read is harmless as long as the
  // address is mapped. Nothing in the runtime maps the 4K-page heap below
  // 0x30000000, so commit the first 64KB granule above the zero page as
  // zero-filled RW memory before the title starts.
  void OnPreLaunchModule() override {
#if !defined(__ANDROID__)
    // Steam (PC) saves become 360 save slots before the game lists its saves (lr_steam_saves.h).
    lr_steam_saves::Import(user_data_root());
#endif
    // PS3 saves dropped into <user data>\ps3_saves become 360 save slots too (lr_ps3_saves.h).
    lr_ps3_saves::Import(user_data_root());
    constexpr uint32_t kLowGranuleBase = 0x00010000;
    constexpr uint32_t kLowGranuleSize = 0x00010000;
    auto* heap = runtime()->memory()->LookupHeap(kLowGranuleBase);
    if (!heap || !heap->AllocFixed(kLowGranuleBase, kLowGranuleSize, kLowGranuleSize,
                                   rex::memory::kMemoryAllocationReserve |
                                       rex::memory::kMemoryAllocationCommit,
                                   rex::memory::kMemoryProtectRead |
                                       rex::memory::kMemoryProtectWrite)) {
      REXLOG_ERROR("Failed to commit guest {:08X}-{:08X} for the early table read",
                   kLowGranuleBase, kLowGranuleBase + kLowGranuleSize - 1);
      return;
    }
    REXLOG_INFO("Committed guest {:08X}-{:08X} for the early table read", kLowGranuleBase,
                kLowGranuleBase + kLowGranuleSize - 1);
  }

  // Button Prompts "Auto": 1 for a PlayStation controller, 0 for an Xbox one, -1 for other kinds.
  // SDL's type first; where the backend gives no type (Android's input devices can come up as
  // "standard"), the USB vendor (Sony 054C, Microsoft 045E) or the name decides.
  static int PadKind(SDL_Gamepad* pad) {
    switch (SDL_GetGamepadType(pad)) {
      case SDL_GAMEPAD_TYPE_PS3:
      case SDL_GAMEPAD_TYPE_PS4:
      case SDL_GAMEPAD_TYPE_PS5:
        return 1;
      case SDL_GAMEPAD_TYPE_XBOX360:
      case SDL_GAMEPAD_TYPE_XBOXONE:
        return 0;
      default:
        break;
    }
    const Uint16 vendor = SDL_GetGamepadVendor(pad);
    if (vendor == 0x054C) return 1;
    if (vendor == 0x045E) return 0;
    const std::string name = SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "";
    if (name.find("Xbox") != std::string::npos) return 0;
    for (const char* ps : {"DualSense", "DualShock", "PS5", "PS4", "PS3", "PlayStation"})
      if (name.find(ps) != std::string::npos) return 1;
    if (name == "Wireless Controller") return 1;  // DualShock 4 / DualSense over Bluetooth on Linux
    return -1;
  }

  // Screenshot on the Xbox Series controller Share/capture button (SDL_GAMEPAD_BUTTON_MISC1)
  // or F12 while the game window has focus: saves the game's output frame as
  // Documents\lightningreturns\screenshots\LR_<time>.png. The runtime's own SDL instance
  // lives inside rexruntime.dll, so this keeps a separate read-only gamepad watcher.
  // Through XInput Windows does not report the Share button. SDL reads it through GameInput
  // (its share-button callback; needs Microsoft's GameInput runtime, which LR_Step53.ps1
  // installs) for USB / wireless-adapter controllers, and through HIDAPI for Bluetooth ones.
  // Set LR_NO_CAPTURE_BUTTON=1 to turn it off.
  void StartCaptureButtonWatcher() {
    if (std::getenv("LR_NO_CAPTURE_BUTTON")) return;
    capture_thread_ = std::thread([this] {
      SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "1");
      SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");  // Bluetooth Xbox Series: Share in the HID report
#if defined(LR_GAMEINPUT_SHARE)
      if (const char* why = lr_gameinput::Start())
        REXLOG_INFO("Capture button: GameInput share button not available: {}", why);
      else
        REXLOG_INFO("Capture button: Share button read through GameInput (API {})", GAMEINPUT_API_VERSION);
#elif defined(_WIN32)
      REXLOG_INFO("Capture button: built without GameInput.h; Share is read only where SDL reports it");
#endif
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        REXLOG_WARN("Capture button: SDL gamepad init failed: {}", SDL_GetError());
        return;
      }
      std::vector<SDL_Gamepad*> pads;
      bool was_down = false;
      bool was_toggle = false;
      // Button Prompts "Auto": the controller that last gave input decides (Xbox -> 360,
      // DualShock / DualSense -> PS3; other kinds don't change it).
      std::unordered_map<SDL_JoystickID, uint64_t> pad_state;
      SDL_JoystickID active_pad = 0;
      ApplySavedUiStyle();
      auto next_scan = std::chrono::steady_clock::now();
      while (!capture_stop_) {
        SDL_UpdateGamepads();
        if (std::chrono::steady_clock::now() >= next_scan) {
          for (auto* pad : pads) SDL_CloseGamepad(pad);
          pads.clear();
          int count = 0;
          if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count; ++i)
              if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
                pads.push_back(pad);
                // Once per controller: does this backend report the Share button at all?
                if (logged_pads_.insert(ids[i]).second) {
                  const char* name = SDL_GetGamepadName(pad);
                  REXLOG_INFO("Capture button: {} ({}, type {}, {:04x}:{:04x}, prompts {}): Share button {}; "
                              "Back + left stick click or F12 also take a screenshot",
                              name ? name : "controller", SDL_GetGamepadPath(pad) ? SDL_GetGamepadPath(pad) : "-",
                              int(SDL_GetGamepadType(pad)), SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad),
                              PadKind(pad) == 1 ? "PS3" : PadKind(pad) == 0 ? "360" : "unchanged",
                              SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_MISC1)
                                  ? "reported"
                                  : "not reported through XInput (needs the GameInput runtime: LR_Step53.ps1 "
                                    "installs it; or connect by Bluetooth)");
                }
              }
            SDL_free(ids);
          }
          UpdateKeyboardMouseMode(pads);
          next_scan = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        }
        for (auto* pad : pads) {
          uint64_t state = 0;
          for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 48; ++b)
            if (SDL_GetGamepadButton(pad, SDL_GamepadButton(b))) state |= uint64_t(1) << b;
          for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && a < 8; ++a)
            if (std::abs(int(SDL_GetGamepadAxis(pad, SDL_GamepadAxis(a)))) > 16000) state |= uint64_t(1) << (48 + a);
          SDL_JoystickID id = SDL_GetGamepadID(pad);
          auto [it, added] = pad_state.try_emplace(id, state);
          bool used = (!added && it->second != state && state) || (added && !active_pad);
          it->second = state;
          if (!used || id == active_pad) continue;
          int kind = PadKind(pad);
          if (kind < 0) continue;
          active_pad = id;
          const char* name = SDL_GetGamepadName(pad);
          LrUiStyleForController(kind, name ? name : (kind ? "PlayStation controller" : "Xbox controller"));
        }
        // Share button, or Back + left stick click (XInput does not report Share, and the
        // Windows Game Bar can keep it for itself).
        bool down = false;
#if defined(LR_GAMEINPUT_SHARE)
        down = lr_gameinput::g_share_down.load(std::memory_order_relaxed);
#endif
        for (auto* pad : pads)
          down |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_MISC1) ||
                  (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
                   SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK));
#if defined(_WIN32)
        if (GetAsyncKeyState(VK_F12) & 0x8000) {
          DWORD pid = 0;
          GetWindowThreadProcessId(GetForegroundWindow(), &pid);
          down |= pid == GetCurrentProcessId();
        }
#endif
        // Also the Android screenshot button (LrTouch.java, through g_lr_screenshot_request).
        bool requested = g_lr_screenshot_request.exchange(false);
        if ((down && !was_down) || requested) {
          SaveScreenshot();
          RequestFrameCapture();
        }
        was_down = down;
        // UI style toggle (PS3 / 360 UI textures): Back + right stick click, or F9.
        bool toggle = false;
        for (auto* pad : pads)
          toggle |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
                    SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
#if defined(_WIN32)
        if (GetAsyncKeyState(VK_F9) & 0x8000) {
          DWORD pid = 0;
          GetWindowThreadProcessId(GetForegroundWindow(), &pid);
          toggle |= pid == GetCurrentProcessId();
        }
#endif
        if (toggle && !was_toggle) ToggleUiStyle();
        was_toggle = toggle;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }
      for (auto* pad : pads) SDL_CloseGamepad(pad);
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    });
  }

  // The renderer plugin exports LrSetUiStyle(int ps3) / LrToggleUiStyle() (texture
  // replacement sets: textures\ps3ui holds the PS3 versions of the UI textures that differ).
  // The choice is kept in Documents\lightningreturns\ui_style.txt; LR_UI_STYLE overrides it.
  template <typename Fn>
  static Fn PluginFunction(const char* name) {
#if defined(_WIN32)
    HMODULE module = GetModuleHandleW(L"rexgpu-xenos.dll");
    return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
#else
    void* module = dlopen("librexgpu-xenos.so", RTLD_NOW | RTLD_NOLOAD);
    return module ? reinterpret_cast<Fn>(dlsym(module, name)) : nullptr;
#endif
  }

  // Voice language and UI style live in lr_hooks.cpp (the game's Config menu changes them);
  // F9 / Back + right stick stays as a shortcut for the UI style.
  void ApplySavedUiStyle() { LrSettingsInit(user_data_root()); }

  void ToggleUiStyle() { LrConfigSetUiStyle(!LrConfigGetUiStyle()); }

  // Keyboard and mouse (PC): the runtime's keyboard/mouse controller (mnk_mode) is on only
  // while no controller is connected, so a controller always wins. Bindings come from
  // keyboard.txt in the settings folder ("keybind_x = M" lines; written on first start with the
  // Steam version's default keys mapped to the controller buttons those actions use).
  static constexpr const char* kKeyboardDefaults =
      "# Lightning Returns - keyboard (used only while no controller is connected).\n"
      "# keyboard layout v3: the Steam (PC) version's default keys (its Settings > Keyboard Settings).\n"
      "# One \"setting = keys\" per line; several keys separated by commas. Key names: A-Z, 0-9,\n"
      "# Space, Return, Backspace, Tab, Escape, Up/Down/Left/Right, Comma, Period, Semicolon,\n"
      "# Quote, F1-F12, Numpad0-Numpad9. A Shift+/Ctrl+/Alt+ prefix requires that modifier.\n"
      "# Move W A S D, camera I J K L, camera reset / camera mode Comma, map reset X\n"
      "keybind_lstick_up = W\n"
      "keybind_lstick_down = S\n"
      "keybind_lstick_left = A\n"
      "keybind_lstick_right = D\n"
      "keybind_rstick_up = I\n"
      "keybind_rstick_down = K\n"
      "keybind_rstick_left = J\n"
      "keybind_rstick_right = L\n"
      "keybind_rstick_press = Comma\n"
      "keybind_lstick_press = X\n"
      "# The four ability keys in battle: Enter (confirm), Backspace (cancel / jump),\n"
      "# M (map), Period (menu)\n"
      "keybind_a = Return\n"
      "keybind_b = Backspace\n"
      "keybind_x = M\n"
      "keybind_y = Period\n"
      "# U attacks to start a battle and, with E, changes schemata; Q Overclock; O Libra / dash\n"
      "keybind_right_shoulder = U\n"
      "keybind_left_shoulder = E\n"
      "keybind_left_trigger = Q\n"
      "keybind_right_trigger = O\n"
      "# Esc pause menu; F1 navigational map on / off (Back); arrow keys cursor / targets (D-pad)\n"
      "keybind_start = Escape\n"
      "keybind_back = F1\n"
      "keybind_dpad_up = Up\n"
      "keybind_dpad_down = Down\n"
      "keybind_dpad_left = Left\n"
      "keybind_dpad_right = Right\n"
      "# The Steam version has no mouse camera; set true to turn the camera with the mouse.\n"
      "mnk_mouse = false\n"
      "mnk_sensitivity = 1.0\n";

  void LoadKeyboardBindings() {
#if !defined(__ANDROID__)
    auto path = user_data_root() / "keyboard.txt";
    std::error_code ec;
    bool write_defaults = !std::filesystem::exists(path, ec);
    if (!write_defaults) {
      // Files from before the Steam layout are replaced (kept as keyboard.txt.old).
      std::ifstream old(path);
      std::string text((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
      if (text.find("keyboard layout v3") == std::string::npos) {
        old.close();
        std::filesystem::rename(path, user_data_root() / "keyboard.txt.old", ec);
        write_defaults = true;
        REXLOG_INFO("LR keyboard: older keyboard.txt kept as keyboard.txt.old; Steam layout written");
      }
    }
    if (write_defaults) {
      if (FILE* f = std::fopen(path.string().c_str(), "w")) {
        std::fputs(kKeyboardDefaults, f);
        std::fclose(f);
      }
    }
    std::ifstream in(path);
    std::string line;
    int applied = 0;
    while (std::getline(in, line)) {
      auto trim = [](std::string v) {
        while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\t')) v.pop_back();
        size_t i = 0;
        while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
        return v.substr(i);
      };
      line = trim(line);
      if (line.empty() || line[0] == '#') continue;
      size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string name = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
      if (name.rfind("keybind_", 0) != 0 && name != "mnk_mouse" && name != "mnk_sensitivity") continue;
      if (rex::cvar::SetFlagByName(name, value)) {
        ++applied;
      } else {
        REXLOG_WARN("LR keyboard: unknown setting '{}' in {}", name, path.string());
      }
    }
    REXLOG_INFO("LR keyboard: {} binding(s) from {}", applied, path.string());
#endif
  }

  void UpdateKeyboardMouseMode(const std::vector<SDL_Gamepad*>& pads) {
#if !defined(__ANDROID__)
    int controllers = 0;
    for (auto* pad : pads) {
      SDL_JoystickID id = SDL_GetGamepadID(pad);
      if (!SDL_IsJoystickVirtual(id)) ++controllers;
    }
    bool want = controllers == 0;
    if (keyboard_mode_known_ && want == keyboard_mode_) return;
    if (!keyboard_mode_known_) LoadKeyboardBindings();
    keyboard_mode_known_ = true;
    keyboard_mode_ = want;
    rex::cvar::SetFlagByName("mnk_mode", want ? "true" : "false");
    g_lr_keyboard_mode.store(want);
    REXLOG_INFO("LR keyboard: keyboard and mouse {} ({} controller(s) connected)",
                want ? "on" : "off", controllers);
#else
    (void)pads;
#endif
  }

  // With LR_CAPTURE_DIR set (Android sets it), the screenshot buttons also ask the renderer to
  // record the next frame (resolves, draws, shaders) for offline inspection.
  void RequestFrameCapture() {
    const char* dir = std::getenv("LR_CAPTURE_DIR");
    if (!dir || !*dir) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (FILE* f = std::fopen((std::string(dir) + "/request").c_str(), "w")) {
      std::fclose(f);
      REXLOG_INFO("LR capture: frame capture requested ({})", dir);
    }
  }

  void SaveScreenshot() {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    rex::ui::RawImage image;
    if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width) {
      REXLOG_WARN("Capture button: no frame to capture");
      return;
    }
    // RawImage is R8G8B8X8; force X to opaque for PNG.
    for (uint32_t y = 0; y < image.height; ++y)
      for (uint32_t x = 0; x < image.width; ++x) image.data[y * image.stride + x * 4 + 3] = 0xFF;
    SDL_Surface* surface = SDL_CreateSurfaceFrom(int(image.width), int(image.height),
                                                 SDL_PIXELFORMAT_RGBA32, image.data.data(),
                                                 int(image.stride));
    if (!surface) return;
    std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char name[64];
    std::strftime(name, sizeof(name), "LR_%Y%m%d_%H%M%S", &local);
    auto dir = user_data_root() / "screenshots";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto path = dir / (std::string(name) + ".png");
    for (int n = 2; std::filesystem::exists(path, ec); ++n)
      path = dir / (std::string(name) + "_" + std::to_string(n) + ".png");
    if (SDL_SavePNG(surface, path.string().c_str())) {
      REXLOG_INFO("Capture button: saved {} ({}x{})", path.string(), image.width, image.height);
    } else {
      REXLOG_WARN("Capture button: could not write {}: {}", path.string(), SDL_GetError());
    }
    SDL_DestroySurface(surface);
  }

  void OnShutdown() override {
    capture_stop_ = true;
    if (capture_thread_.joinable()) capture_thread_.join();
    // Before the runtime destroys the ImGui drawer the overlay is registered with.
    rex::ui::UnregisterBind("bind_lr_frame_overlay");
    frame_overlay_.reset();
    if (window()) window()->RemoveInputListener(&key_listener_);
    g_lr_controls_ui = nullptr;
    controls_overlay_.reset();
    controls_picture_.texture.reset();
  }

  // F2: frame rate and frame time graph (rebindable as bind_lr_frame_overlay in the F4 settings).
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    rex::ui::RegisterBind("bind_lr_frame_overlay", "F2", "Frame rate and frame time graph", [this, drawer] {
      if (frame_overlay_) {
        frame_overlay_.reset();
      } else {
        frame_overlay_ = std::make_unique<LrFrameOverlay>(drawer);
      }
      REXLOG_INFO("LR frame overlay: {}", frame_overlay_ ? "on" : "off");
    });
#if !defined(__ANDROID__)
    if (window()) window()->AddInputListener(&key_listener_, SIZE_MAX);
    // The mouse cursor hides after 5 s without movement, as in the PC version (the runtime's
    // auto-hide; keyboard mode's mouse capture hides it outright).
    if (window() && window()->GetCursorVisibility() == rex::ui::Window::CursorVisibility::kVisible) {
      window()->SetCursorAutoHideDelayMs(5000);
      window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
    }
    g_lr_controls_ui = [this, drawer](bool show) {
      app_context().CallInUIThread([this, drawer, show] {
        if (!show) {
          controls_overlay_.reset();
        } else if (!controls_overlay_) {
          controls_overlay_ = std::make_unique<LrControlsOverlay>(drawer, immediate_drawer(), &controls_picture_);
        }
      });
    };
#endif
  }

  std::unique_ptr<LrFrameOverlay> frame_overlay_;
  LrKeyListener key_listener_;
  LrControlsPictureTexture controls_picture_;
  std::unique_ptr<LrControlsOverlay> controls_overlay_;

  std::thread capture_thread_;
  std::unordered_set<SDL_JoystickID> logged_pads_;  // controllers already described in the log
  std::atomic<bool> capture_stop_{false};
  bool keyboard_mode_known_ = false;  // UpdateKeyboardMouseMode (capture thread only)
  bool keyboard_mode_ = false;

  void OnPostLaunchModule(rex::system::XThread*) override { StartCaptureButtonWatcher(); }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.audio_factory = REX_AUDIO_BACKEND(LrAudioSystem);
    // Render resolution (Settings > Resolution, LR_Play.ps1 -Resolution): read by the renderer
    // plugin when it is loaded, right after this.
    LrResolutionStartup(user_data_root());
    // 120 fps (Settings > Frame Rate, PC only): the runtime's vertical blank runs at 120 Hz.
    LrFrameRateStartup();
  }

  // After the graphics are set up, before the game starts: with 120 fps, the game is told
  // 60 Hz again (the vertical blank stays at 120 Hz).
  void OnPostSetup() override { LrFrameRateAfterSetup(); }

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // Started without arguments (double-clicked): the folders come from lightningreturns_launch.txt
  // next to the exe (the builder writes it: "game_data_root = <360 game folder>"; also
  // update_data_root and LR_* settings), else from the folders next to the exe - game\, jpvoice\
  // (Japanese voices), movies_ps3\ (PS3 cinematics), textures\ (PS3 button / UI set). Command
  // line arguments and environment variables set by a launcher (LR_Play.ps1) win over both.
  void OnConfigurePaths(rex::PathConfig& paths) override {
#if defined(_WIN32)
    const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
    auto resolve = [&](const std::string& v) {
      std::filesystem::path p = std::filesystem::u8path(v);
      return p.is_absolute() ? p : exe_dir / p;
    };
    // _putenv_s, not rex::platform::env::set (SetEnvironmentVariableW): the game code and the
    // renderer read these with getenv, which only sees the C runtime's copy of the environment.
    auto set_env = [](const char* name, const std::string& value) {
      const char* cur = std::getenv(name);
      if (!cur || !*cur) _putenv_s(name, value.c_str());
    };
    std::ifstream launch(exe_dir / "lightningreturns_launch.txt");
    std::string line;
    while (std::getline(launch, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      size_t eq = line.find('=');
      if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
      std::string key = line.substr(0, eq), value = line.substr(eq + 1);
      while (!key.empty() && key.back() == ' ') key.pop_back();
      while (!value.empty() && value.front() == ' ') value.erase(0, 1);
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
      if (key == "game_data_root" && paths.game_data_root.empty()) {
        paths.game_data_root = resolve(value);
      } else if (key == "update_data_root" && paths.update_data_root.empty()) {
        paths.update_data_root = resolve(value);
      } else if (key.rfind("LR_", 0) == 0) {
        set_env(key.c_str(), value);
      }
    }
    std::error_code ec;
    if (paths.game_data_root.empty() && std::filesystem::exists(exe_dir / "game" / "default.xex", ec)) {
      paths.game_data_root = exe_dir / "game";
    }
    const std::filesystem::path jp = exe_dir / "jpvoice";
    if (paths.update_data_root.empty() && std::filesystem::exists(jp / "filelist_patch_u.x360.bin", ec)) {
      paths.update_data_root = jp;
      set_env("LR_JP_TEXT_MAP", (jp / "jp_text.txt").string());
      set_env("LR_JP_LIPSYNC_LIST", (jp / "jp_lipsync.txt").string());
    }
    if (std::filesystem::is_directory(exe_dir / "movies_ps3", ec)) set_env("LR_PS3_MOVIES", (exe_dir / "movies_ps3").string());
    if (std::filesystem::is_directory(exe_dir / "textures", ec)) set_env("LR_TEXTURES", (exe_dir / "textures").string());
#else
    (void)paths;
#endif
  }
};
