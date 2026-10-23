// lightningreturns - keyboard key icons in the game's button prompts (as the Steam version).
//
// The game draws button icons inside text ("{f1}@" = A, ...) from one 128x128 sheet,
// button_set01 (the GUI manager [0x831203AC] keeps it at +0x6C8), through three tables of
// 32-byte entries {x, y, width, height (screen units), u0, v0, u1, v1} (floats), one entry per
// code 0x40.. (the second byte after 0xF1):
//   0x8214BB10  the icon itself (all four d-pad directions share one cell, as do the sticks')
//   0x8214C350  an overlay drawn on top (direction highlight, stick arrow, START / BACK text)
//   0x8214C770  the stick letters (L / R), six entries shared by the stick codes
// The Steam version shows, for each code, the key bound to that button (Settings > Keyboard
// Settings). Here: while keyboard mode is on, the tables point every code at its own cell in
// the sheet's empty space, the overlays are switched off, and the renderer swaps the sheet for
// an 8x copy with the bound keys' icons in those cells (Steam's key icon sheet, button_En: 20x20
// px per key at (DIK % 16, DIK / 16), BGRA - steam_ui/button_En.bgra in the settings folder,
// copied from the Steam install by the step script). The original art stays where it was, so
// the battle HUD and every other user of the sheet look as before.
//
// Codes and buttons (the Steam text's command list, $f_con_83..106):
//   @ A, A B, B X, C Y, D START, E BACK, F LB, G RB, H LT, I RT, J/K/L/M d-pad left/down/right/up,
//   N/O/P/Q left stick left/down/right/up, R/S left stick left-right / up-down, T L3, U R3,
//   V/W/X/Y right stick left/down/right/up, Z/[ right stick left-right / up-down, \ left stick,
//   ] right stick, ^ d-pad left-right, _ d-pad up-down, ` d-pad.

#include <rex/cvar.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kIconTable = 0x8214BB10;
constexpr uint32_t kIconOverlays = 0x8214C350;
constexpr uint32_t kIconLetters = 0x8214C770;
constexpr uint32_t kIconLetterCount = 6;
constexpr uint32_t kIconFirst = 0x40, kIconCount = 33;  // 0x40..0x60
constexpr uint32_t kGuiButtonSheet = 0x6C8;
constexpr uint32_t kSheetSize = 128, kSheetScale = 8;
// The guest sheet's hashes as the renderer computes them (untiled base level), for the
// replacement: the sheet as loaded, and as the PS3 UI set names it.
constexpr uint64_t kSheetHashes[] = {0x876594D08DDA757Bull, 0x922537DE69485257ull};

// The 24 key slots of the Steam version's keyboard settings, in its order.
constexpr const char* kKeySlotCvars[24] = {
    "keybind_lstick_up",   "keybind_lstick_down", "keybind_lstick_left",   "keybind_lstick_right",
    "keybind_dpad_up",     "keybind_dpad_down",   "keybind_dpad_left",     "keybind_dpad_right",
    "keybind_rstick_up",   "keybind_rstick_down", "keybind_rstick_left",   "keybind_rstick_right",
    "keybind_a",           "keybind_b",           "keybind_y",             "keybind_x",
    "keybind_left_shoulder", "keybind_right_shoulder", "keybind_left_trigger", "keybind_right_trigger",
    "keybind_start",       "keybind_back",        "keybind_lstick_press",  "keybind_rstick_press"};

struct LrIconCode {
  uint8_t keys;      // 1, 2 (side by side, twice as wide), or 4 (2x2: up left / down right)
  int8_t slot[4];
};
// Index = code - 0x40.
constexpr LrIconCode kIconCodes[kIconCount] = {
    {1, {12}}, {1, {13}}, {1, {15}}, {1, {14}},              // A B X Y
    {1, {20}}, {1, {21}},                                    // START BACK
    {1, {16}}, {1, {17}}, {1, {18}}, {1, {19}},              // LB RB LT RT
    {1, {6}},  {1, {5}},  {1, {7}},  {1, {4}},               // d-pad left down right up
    {1, {2}},  {1, {1}},  {1, {3}},  {1, {0}},               // left stick left down right up
    {2, {2, 3}}, {2, {0, 1}},                                // left stick left-right, up-down
    {1, {22}}, {1, {23}},                                    // L3 R3
    {1, {10}}, {1, {9}},  {1, {11}}, {1, {8}},               // right stick left down right up
    {2, {10, 11}}, {2, {8, 9}},                              // right stick left-right, up-down
    {4, {0, 2, 1, 3}}, {4, {8, 10, 9, 11}},                  // left stick, right stick
    {2, {6, 7}}, {2, {4, 5}},                                // d-pad left-right, up-down
    {4, {4, 6, 5, 7}},                                       // d-pad
};

// DirectInput scan code of a key name as keyboard.txt / the keybind cvars spell it (the first
// alternative of a binding, modifiers dropped); 0 = none or unknown.
uint8_t lr_dik_for(std::string_view name) {
  static const std::pair<const char*, uint8_t> kNames[] = {
      {"Escape", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06}, {"6", 0x07},
      {"7", 0x08}, {"8", 0x09}, {"9", 0x0A}, {"0", 0x0B}, {"Minus", 0x0C}, {"Plus", 0x0D},
      {"Backspace", 0x0E}, {"Tab", 0x0F}, {"Q", 0x10}, {"W", 0x11}, {"E", 0x12}, {"R", 0x13},
      {"T", 0x14}, {"Y", 0x15}, {"U", 0x16}, {"I", 0x17}, {"O", 0x18}, {"P", 0x19},
      {"LBracket", 0x1A}, {"RBracket", 0x1B}, {"Return", 0x1C}, {"Control", 0x1D}, {"A", 0x1E},
      {"S", 0x1F}, {"D", 0x20}, {"F", 0x21}, {"G", 0x22}, {"H", 0x23}, {"J", 0x24}, {"K", 0x25},
      {"L", 0x26}, {"Semicolon", 0x27}, {"Quote", 0x28}, {"Backtick", 0x29}, {"Shift", 0x2A},
      {"Backslash", 0x2B}, {"Z", 0x2C}, {"X", 0x2D}, {"C", 0x2E}, {"V", 0x2F}, {"B", 0x30},
      {"N", 0x31}, {"M", 0x32}, {"Comma", 0x33}, {"Period", 0x34}, {"Slash", 0x35}, {"Alt", 0x38},
      {"Space", 0x39}, {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4", 0x3E}, {"F5", 0x3F},
      {"F6", 0x40}, {"F7", 0x41}, {"F8", 0x42}, {"F9", 0x43}, {"F10", 0x44}, {"Numpad7", 0x47},
      {"Numpad8", 0x48}, {"Numpad9", 0x49}, {"Numpad4", 0x4B}, {"Numpad5", 0x4C}, {"Numpad6", 0x4D},
      {"Numpad1", 0x4F}, {"Numpad2", 0x50}, {"Numpad3", 0x51}, {"Numpad0", 0x52}, {"F11", 0x57},
      {"F12", 0x58}, {"F13", 0x64}, {"F14", 0x65}, {"F15", 0x66}, {"NumpadEnter", 0x9C},
      {"Home", 0xC7}, {"Up", 0xC8}, {"PageUp", 0xC9}, {"Left", 0xCB}, {"Right", 0xCD}, {"End", 0xCF},
      {"Down", 0xD0}, {"PageDown", 0xD1}, {"Insert", 0xD2}, {"Delete", 0xD3}};
  name = name.substr(0, name.find(','));
  if (size_t plus = name.rfind('+'); plus != std::string_view::npos && plus + 1 < name.size()) {
    name = name.substr(plus + 1);
  }
  while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
  while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
  for (const auto& [n, dik] : kNames) {
    if (name.size() == std::strlen(n) && std::equal(name.begin(), name.end(), n, [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        })) {
      return dik;
    }
  }
  return 0;
}

struct LrIconCell {
  float x, y, w, h;  // sheet texels (of 128)
};

// Cells in the sheet's empty space: a 7x4 grid of 5-texel cells at (48.5, 104.5), 3x3 at
// (80.5, 48.5), and 3x2 at (68.5, 72.5). Two-key codes take two cells side by side.
bool lr_icon_layout(LrIconCell (&cells)[kIconCount]) {
  struct Block {
    float x, y;
    int cols, rows;
  };
  constexpr Block kBlocks[] = {{48.5f, 104.5f, 7, 4}, {80.5f, 48.5f, 3, 3}, {68.5f, 72.5f, 3, 2}};
  constexpr float kCell = 5.0f;
  bool used[3][7][4] = {};
  for (int pass = 0; pass < 2; ++pass) {  // wide cells first
    for (uint32_t i = 0; i < kIconCount; ++i) {
      int span = kIconCodes[i].keys == 2 ? 2 : 1;
      if ((pass == 0) != (span == 2)) continue;
      bool placed = false;
      for (int b = 0; b < 3 && !placed; ++b)
        for (int r = 0; r < kBlocks[b].rows && !placed; ++r)
          for (int c = 0; c + span <= kBlocks[b].cols && !placed; ++c) {
            if (used[b][c][r] || (span == 2 && used[b][c + 1][r])) continue;
            for (int s = 0; s < span; ++s) used[b][c + s][r] = true;
            cells[i] = {kBlocks[b].x + c * kCell, kBlocks[b].y + r * kCell, kCell * span, kCell};
            placed = true;
          }
      if (!placed) return false;
    }
  }
  return true;
}

// Bilinear copy of src (BGRA, sw x sh, row-major) rectangle (sx, sy, sw_r, sh_r) into dst (BGRA,
// dw wide) rectangle (dx, dy, w, h), pixels.
void lr_blit(const uint8_t* src, uint32_t sw, uint32_t sh, float sx, float sy, float sw_r, float sh_r,
             uint8_t* dst, uint32_t dw, uint32_t dh, int dx, int dy, int w, int h) {
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int ox = dx + x, oy = dy + y;
      if (ox < 0 || oy < 0 || uint32_t(ox) >= dw || uint32_t(oy) >= dh) continue;
      float u = sx + (x + 0.5f) * sw_r / w - 0.5f, v = sy + (y + 0.5f) * sh_r / h - 0.5f;
      u = std::clamp(u, sx, sx + sw_r - 1.0f);
      v = std::clamp(v, sy, sy + sh_r - 1.0f);
      int x0 = int(u), y0 = int(v);
      int x1 = std::min<int>(x0 + 1, int(sw) - 1), y1 = std::min<int>(y0 + 1, int(sh) - 1);
      float fx = u - x0, fy = v - y0;
      for (int ch = 0; ch < 4; ++ch) {
        auto at = [&](int px, int py) { return float(src[(size_t(py) * sw + px) * 4 + ch]); };
        float val = (1 - fy) * ((1 - fx) * at(x0, y0) + fx * at(x1, y0)) + fy * ((1 - fx) * at(x0, y1) + fx * at(x1, y1));
        dst[(size_t(oy) * dw + ox) * 4 + ch] = uint8_t(std::clamp(val + 0.5f, 0.0f, 255.0f));
      }
    }
  }
}

// Xenos 2D tiled address of texel (x, y) (bytes per texel 4).
uint32_t lr_tiled_offset(uint32_t x, uint32_t y, uint32_t pitch) {
  pitch = (pitch + 31) & ~31u;
  const uint32_t bpb_log2 = 2;
  uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7);
  uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

struct LrKeyIcons {
  bool applied = false;       // tables point at the key cells, renderer has the sheet
  bool unavailable = false;   // no key icon sheet: keep the controller icons
  uint64_t bindings = 0;      // hash of the bindings the sheet shows
  uint8_t backup_icons[kIconCount * 32];
  uint8_t backup_overlays[kIconCount * 32];
  uint8_t backup_letters[kIconLetterCount * 32];
  std::vector<uint8_t> key_sheet;  // 512x512 BGRA
  int frames = 0;
};
LrKeyIcons g_lr_key_icons;

bool lr_tables_writable() {
  static int state = -1;
  if (state < 0) {
    auto* ks = REX_KERNEL_STATE();
    auto* heap = ks && ks->memory() ? ks->memory()->LookupHeap(kIconTable) : nullptr;
    uint32_t old = 0;
    state = heap && heap->Protect(kIconTable & ~0xFFFu, 0x2000,
                                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, &old)
                ? 1
                : 0;
    if (!state) REXLOG_WARN("LR keyboard icons: the icon tables could not be made writable");
  }
  return state == 1;
}

void lr_put_float(uint8_t* base, uint32_t address, float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  st32(base, address, u);
}

// Builds the sheet for the current bindings and points the tables at it.
bool lr_key_icons_apply(uint8_t* base, const uint8_t (&diks)[24]) {
  using SetIconsFn = int (*)(const uint8_t*, uint32_t, uint32_t, const uint64_t*, int, uint32_t, uint32_t);
  static const SetIconsFn set_icons = lr_plugin_function<SetIconsFn>("LrSetKeyboardIcons");
  uint32_t gui = ld32(base, kGuiManager);
  uint32_t sheet = gui ? ld32(base, gui + kGuiButtonSheet) : 0;
  uint32_t header = sheet ? ld32(base, sheet + 0x38) : 0;
  if (!set_icons || !header || !lr_tables_writable()) return false;
  uint32_t fetch1 = ld32(base, header + 0x5C), fetch2 = ld32(base, header + 0x60);
  uint32_t guest = fetch1 & ~0xFFFu;
  if ((fetch1 & 0x3F) != 6 || (fetch2 & 0x1FFF) != kSheetSize - 1) {
    REXLOG_WARN("LR keyboard icons: unexpected button sheet (fetch {:08X} {:08X})", fetch1, fetch2);
    return false;
  }
  // The guest sheet, untiled, as BGRA (the guest stores big-endian ARGB words).
  std::vector<uint8_t> orig(kSheetSize * kSheetSize * 4);
  for (uint32_t y = 0; y < kSheetSize; ++y)
    for (uint32_t x = 0; x < kSheetSize; ++x) {
      const uint8_t* p = base + guest + lr_tiled_offset(x, y, kSheetSize);
      uint8_t* o = &orig[(y * kSheetSize + x) * 4];
      o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0];
    }
  const uint32_t size = kSheetSize * kSheetScale;
  std::vector<uint8_t> out(size_t(size) * size * 4);
  lr_blit(orig.data(), kSheetSize, kSheetSize, 0, 0, float(kSheetSize), float(kSheetSize), out.data(), size, size, 0,
          0, int(size), int(size));
  LrIconCell cells[kIconCount];
  if (!lr_icon_layout(cells)) return false;
  const float s = float(kSheetScale);
  for (uint32_t i = 0; i < kIconCount; ++i) {
    const LrIconCode& code = kIconCodes[i];
    const LrIconCell& c = cells[i];
    // Clear the cell (it is empty in the original, but the bilinear upscale bleeds into it).
    for (int y = int(c.y * s); y < int((c.y + c.h) * s); ++y)
      std::memset(&out[(size_t(y) * size + int(c.x * s)) * 4], 0, size_t(c.w * s) * 4);
    for (int k = 0; k < code.keys; ++k) {
      uint8_t dik = diks[code.slot[k]];
      if (!dik) continue;
      float kx = c.x, ky = c.y, kw = c.w / (code.keys == 2 ? 2 : 1), kh = c.h;
      if (code.keys == 2) kx += kw * k;
      if (code.keys == 4) {
        kw = c.w / 2;
        kh = c.h / 2;
        kx += kw * (k & 1);
        ky += kh * (k >> 1);
      }
      const float inset = code.keys == 4 ? 0.0f : 0.4f;
      lr_blit(g_lr_key_icons.key_sheet.data(), 512, 512, float((dik % 16) * 20), float((dik / 16) * 20), 20, 20,
              out.data(), size, size, int((kx + inset) * s), int((ky + inset) * s), int((kw - 2 * inset) * s),
              int((kh - 2 * inset) * s));
    }
  }
  if (set_icons(out.data(), size, size, kSheetHashes, int(std::size(kSheetHashes)), kSheetSize, kSheetSize) <= 0) {
    return false;
  }
  if (!g_lr_key_icons.applied) {
    std::memcpy(g_lr_key_icons.backup_icons, base + kIconTable, sizeof(g_lr_key_icons.backup_icons));
    std::memcpy(g_lr_key_icons.backup_overlays, base + kIconOverlays, sizeof(g_lr_key_icons.backup_overlays));
    std::memcpy(g_lr_key_icons.backup_letters, base + kIconLetters, sizeof(g_lr_key_icons.backup_letters));
  }
  for (uint32_t i = 0; i < kIconCount; ++i) {
    const LrIconCell& c = cells[i];
    uint32_t e = kIconTable + i * 32;
    // Inset half a texel of the 8x sheet so bilinear filtering stays inside the cell.
    const float half = 0.5f / s;
    float values[8] = {0.0f, 0.0f, kIconCodes[i].keys == 2 ? 36.0f : 18.0f, 18.0f,
                       (c.x + half) / kSheetSize, (c.y + half) / kSheetSize,
                       (c.x + c.w - half) / kSheetSize, (c.y + c.h - half) / kSheetSize};
    for (int k = 0; k < 8; ++k) lr_put_float(base, e + k * 4, values[k]);
    std::memset(base + kIconOverlays + i * 32, 0, 32);
  }
  std::memset(base + kIconLetters, 0, kIconLetterCount * 32);
  g_lr_key_icons.applied = true;
  return true;
}

void lr_key_icons_restore(uint8_t* base) {
  using SetIconsFn = int (*)(const uint8_t*, uint32_t, uint32_t, const uint64_t*, int, uint32_t, uint32_t);
  static const SetIconsFn set_icons = lr_plugin_function<SetIconsFn>("LrSetKeyboardIcons");
  if (!g_lr_key_icons.applied) return;
  std::memcpy(base + kIconTable, g_lr_key_icons.backup_icons, sizeof(g_lr_key_icons.backup_icons));
  std::memcpy(base + kIconOverlays, g_lr_key_icons.backup_overlays, sizeof(g_lr_key_icons.backup_overlays));
  std::memcpy(base + kIconLetters, g_lr_key_icons.backup_letters, sizeof(g_lr_key_icons.backup_letters));
  if (set_icons) set_icons(nullptr, 0, 0, nullptr, 0, 0, 0);
  g_lr_key_icons.applied = false;
  g_lr_key_icons.bindings = 0;
}

bool lr_key_sheet_load() {
  if (!g_lr_key_icons.key_sheet.empty()) return true;
  if (g_lr_key_icons.unavailable) return false;
  std::vector<std::filesystem::path> candidates;
  if (const char* env = std::getenv("LR_KEY_ICONS"); env && env[0]) candidates.emplace_back(env);
  if (!g_lr_settings_dir.empty()) candidates.push_back(g_lr_settings_dir / "steam_ui" / "button_En.bgra");
  for (const auto& path : candidates) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() == 512 * 512 * 4) {
      g_lr_key_icons.key_sheet = std::move(data);
      REXLOG_INFO("LR keyboard icons: key icons from {}", path.string());
      return true;
    }
  }
  g_lr_key_icons.unavailable = true;
  REXLOG_INFO("LR keyboard icons: no Steam key icon sheet (steam_ui/button_En.bgra); prompts keep the "
              "controller icons");
  return false;
}

}  // namespace

// Once per frame on the main thread (the frame-rate hook): keeps the prompts in step with
// keyboard mode and the bindings.
bool LrKeyboardViewActive();  // lr_keyboard_menu.inc

// Keys in the prompts while playing with the keyboard, and always in Settings > Keyboard
// Settings (as in the Steam version, whatever is connected).
void LrKeyIconsFrame(uint8_t* base) {
  bool want = g_lr_keyboard_mode.load(std::memory_order_relaxed) || LrKeyboardViewActive();
  if (!want) {
    if (g_lr_key_icons.applied) {
      lr_key_icons_restore(base);
      REXLOG_INFO("LR keyboard icons: off (controller icons)");
    }
    return;
  }
  if (++g_lr_key_icons.frames % 30 != 1 && g_lr_key_icons.applied) return;  // bindings: twice a second
  uint8_t diks[24];
  uint64_t hash = 1469598103934665603ull;
  for (int i = 0; i < 24; ++i) {
    diks[i] = lr_dik_for(rex::cvar::GetFlagByName(kKeySlotCvars[i]));
    hash = (hash ^ diks[i]) * 1099511628211ull;
  }
  if (g_lr_key_icons.applied && hash == g_lr_key_icons.bindings) return;
  if (!lr_key_sheet_load()) return;
  if (lr_key_icons_apply(base, diks)) {
    g_lr_key_icons.bindings = hash;
    REXLOG_INFO("LR keyboard icons: prompts show the keys ({:02X} {:02X} {:02X} {:02X} ...)", diks[12], diks[13],
                diks[14], diks[15]);
  }
}
