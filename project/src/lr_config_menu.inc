// lightningreturns - four extra items in the game's own Settings menu (#MainConfigMenu):
//   "Voice Language"  English / Japanese (voices and lip-sync) -> LrConfigSetVoice
//   "Text Language"   English / Japanese (from the next start) -> LrConfigSetText
//   "Button Prompts"  Auto / Xbox 360 / PlayStation 3        -> LrConfigSetUiMode
//                     (Auto follows the controller in use: Xbox -> 360, DualShock /
//                     DualSense -> PS3; lightningreturns_app.h watches the pads)
//   "Frame Rate"      30 fps / 60 fps (gameplay) / 60 fps (all) -> LrConfigSetFps60 (live)
//   "Anti-Aliasing"   Off / FXAA / FXAA Extreme (live)        -> LrConfigSetAa
//   "Cheats"          Off / Free Teleport & Chronostasis / Freeze Clock / Both -> LrConfigSetCheats
//   "Resolution"      Auto / 720p / 1440p / 2160p (4K)       -> LrConfigSetResolution
//                     (the renderer's scale is fixed while it runs: applies at the next start,
//                     which includes the game's restart on returning to the title screen)
//
// The items are ordinary entries of the game's config list: same rows, cursor, scrolling,
// help bar, value popup (A opens it, Up/Down + A picks, B cancels), sounds, and fonts. Only
// the item ids are new; every place that turns an item id into game data is hooked below.
//
// How the game's config menu works (static analysis of the 360 executable):
//   MainConfigMenu   ctor sub_8278FF28, vtable 0x8211C138, input callback sub_8278F268
//                    (A -> sub_8278EB38, B -> sub_8278C358, X/Y -> restore defaults,
//                    Up/Down -> cursor, Left/Right -> page up/down), step sub_8278F7F0.
//                    +0x9C ConfigList, +0xA0 open value popup, +0xAC scroll bar.
//   ConfigList       ctor sub_82791388 (base sub_82790C90), 0x280 bytes, 16 row widgets at
//                    +0x1BC (15 visible + 1 for scrolling), +0x1B0 first shown index,
//                    +0x1B8 cursor index. Its item list is embedded at +0xE0:
//   item list        built by sub_8278D648 from the order table at 0x8211C0F0 (18 ids):
//                    +0x00 u32 id[21], +0x54 u32 value[21], +0xA8 u32 count (at most 17
//                    of the 18 ids pass the game's filters, so two more always fit).
//   row fill         sub_8278EEB0(list, row, index): label "$f_con_%02d" % base[id] and
//                    value "$f_con_%02d" % (base[id] + 1 + value), base table 0x8211B580.
//   value popup      ctor sub_8278C9F8(popup, id, value), 0x110 bytes, vtable 0x8211BF08:
//                    +0x90 id, +0xF0 hovered value, +0xF4 value count (4 for id 0, 3 for
//                    id 12, 2 otherwise), +0xF8 result (-1 = cancelled), +0x104 id again.
//                    Value texts come from sub_8278AA78(buf, 16, number) ("$f_con_%02d"),
//                    number = base[id] + 1 + value, for id 0 the table at 0x8211B5D4
//                    (2, 1, 4, 3). The rows are laid out for the count (sub_8278C528), and
//                    after construction only +0x90 (read by sub_8278F7F0 on close) depends on
//                    the id - so a 4-value item is built as id 0 and gets its id back.
//   help bar         sub_8278DC38 (menu, "$f_con_%02dh" % base[id]) and sub_8278B4D0
//                    (popup, "$f_con_%02dh" % (base[id] + 1 + hovered)), both through
//                    sub_8270C488([0x831203AC], key, 0, 0).
//   apply            on popup close sub_8278F7F0 calls sub_8272FA78(id, value) (store into
//                    the "gameconfig" save block [0x8312E7D0]+0x2C..+0x3B) and
//                    sub_8272F8C0(id) (side effects). Restore defaults (sub_8278F4A8) uses
//                    sub_8272F598(id) for each listed id, then the same two calls.
//   text             every '$' key goes through LocalizedText::Set (sub_82729450), which
//                    first asks sub_8270CA90(gui, key): a heap copy of the text (game heap
//                    sub_827223F0, the caller frees it with sub_827224E8) or 0.
//
// The values are not kept in the game's save data (that block is 16 bytes and its
// flags word is shared with the game's own options); LrConfigGet*/Set* hold them.
// LR_CONFIG_MENU_ITEMS=0 leaves the menu as the game built it.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/ppc/func.h>

// Keyboard Settings view (lr_keyboard_menu.inc).
bool LrKeyboardActivate(PPCContext& ctx, uint8_t* base, uint32_t menu, uint32_t id);
bool LrKeyboardRowTexts(uint8_t* base, uint32_t id, std::string& label, std::string& value);
const char* LrKeyboardHelp();
bool LrKeyboardViewOpen(uint32_t menu);
bool LrKeyboardDialogYes(PPCContext& ctx, uint8_t* base, uint32_t menu);
void LrKeyboardStep(PPCContext& ctx, uint8_t* base, uint32_t menu);
bool LrControlsOpen();

// Backend (lr_hooks.cpp).
int LrConfigGetVoice();
void LrConfigSetVoice(int jp);
int LrConfigGetText();
void LrConfigSetText(int jp);
int LrConfigGetUiMode();         // 0 auto, 1 Xbox 360, 2 PlayStation 3
void LrConfigSetUiMode(int mode);
int LrConfigVoiceAvailable();
int LrConfigTextAvailable();  // 1 when the Japanese text is installed  // 1 installed, 0 not installed, -1 not known yet
int LrConfigGetFps60();         // 0: 30 fps; 1: 60 fps, cutscenes 30; 2: 60 fps everywhere
void LrConfigSetFps60(int choice);
int LrConfigGetAa();               // 0 off, 1 FXAA, 2 FXAA extreme
int LrConfigGetCheats();           // bit 0 free Teleport / Chronostasis, bit 1 freeze the clock
void LrConfigSetCheats(int choice);
void LrConfigSetAa(int choice);
int LrConfigGetConfirm();         // 1: B confirms and A cancels (the PC's other layout)
void LrConfigSetConfirm(int b);
int LrConfigGetResolution();    // 0 auto, 1 720p, 2 1440p, 3 2160p
bool LrConfigSetResolution(int choice);  // stores it; true: a restart is needed to apply it
void LrResolutionRestartNow();          // restarts the game (returns if it could not)
int LrResolutionScaleFor(int choice);  // 1..3, 0 = not known
int LrResolutionActiveScale();         // what the renderer uses now

namespace {

constexpr uint32_t kGuiManager = 0x831203AC;    // [kGuiManager] = GUI manager ("gui")
constexpr uint32_t kBaseTextTable = 0x8211B580; // u32 per id: first "$f_con_NN" number

// Item list (ConfigList + 0xE0).
constexpr uint32_t kListIds = 0x00;
constexpr uint32_t kListValues = 0x54;
constexpr uint32_t kListCount = 0xA8;
constexpr uint32_t kListCapacity = 21;          // 21 slots in the game's arrays

// ConfigList / MainConfigMenu / popup fields.
constexpr uint32_t kConfigListItems = 0xE0;
constexpr uint32_t kConfigListCount = 0xE0 + kListCount;  // 0x188
constexpr uint32_t kConfigListScrolling = 0x1A4;
constexpr uint32_t kConfigListCursor = 0x1B8;
constexpr uint32_t kMenuList = 0x9C;
constexpr uint32_t kPopupId = 0x90;
constexpr uint32_t kPopupHovered = 0xF0;
constexpr uint32_t kPopupId2 = 0x104;
constexpr uint32_t kId0ValueTexts = 0x8211B5D4;  // id 0's value text numbers, one per value

// Game ids are 0..20 (switches in sub_8272F250 / sub_8272F598 / sub_8272FA78 cover 0..20);
// these two take the default case everywhere the hooks below do not catch them.
constexpr uint32_t kIdVoice = 0x60;
constexpr uint32_t kIdUiStyle = 0x61;
constexpr uint32_t kIdResolution = 0x62;
constexpr uint32_t kIdFps = 0x63;
constexpr uint32_t kIdKeyboard = 0x64;  // Keyboard Settings (opens its own view, PC only)
constexpr uint32_t kIdGamepad = 0x65;
constexpr uint32_t kIdText = 0x68;     // Text Language: English / Japanese
constexpr uint32_t kIdAa = 0x69;       // Anti-Aliasing: Off / FXAA / FXAA Extreme
constexpr uint32_t kIdCheats = 0x6A;   // Cheats: Off / Free Teleport & Chronostasis / Freeze Clock / Both
constexpr uint32_t kIdConfirm = 0x67;  // Confirm Button: A / B (the PC's Control_ConfirmButtonLayout)   // Gamepad Controls (shows the controls picture)

// Settings restored by the menu's "Restore defaults".
constexpr int kDefaultVoiceJp = 0;  // English, as the 360 release
constexpr int kDefaultUiMode = 0;   // Auto (follows the controller)
constexpr int kDefaultResolution = 0;  // Auto
constexpr int kDefaultFps60 = 0;       // 30 fps, as the 360 release

constexpr uint32_t kSoundBuzzer = 0x49;  // sub_82715CD0 sound ids: 4 confirm, 3 cancel, 5 cursor

struct LrItem {
  uint32_t id;
  const char* label;        // keys, at most 15 characters (the game's key buffers are 16)
  const char* help;
  const char* help_unavailable;
  uint32_t count;           // 2, 3 (built as the game's id 12), or 4 (built as id 0), see the
                            // value popup
  const char* value[4];
  const char* value_help[4];
};

constexpr LrItem kItems[] = {
    {kIdVoice, "$lr_voice", "$lr_voiceh", "$lr_voiceu", 2,
     {"$lr_voice_0", "$lr_voice_1"}, {"$lr_voice_0h", "$lr_voice_1h"}},
    {kIdText, "$lr_text", "$lr_texth", "$lr_textu", 2,
     {"$lr_text_0", "$lr_text_1"}, {"$lr_text_0h", "$lr_text_1h"}},
    {kIdUiStyle, "$lr_ui", "$lr_uih", nullptr, 3,
     {"$lr_ui_a", "$lr_ui_0", "$lr_ui_1"}, {"$lr_ui_ah", "$lr_ui_0h", "$lr_ui_1h"}},
    {kIdConfirm, "$lr_cfm", "$lr_cfmh", nullptr, 2,
     {"$lr_cfm_0", "$lr_cfm_1"}, {"$lr_cfm_0h", "$lr_cfm_1h"}},
    {kIdFps, "$lr_fps", "$lr_fpsh", nullptr, 3,
     {"$lr_fps_0", "$lr_fps_1", "$lr_fps_2"}, {"$lr_fps_0h", "$lr_fps_1h", "$lr_fps_2h"}},
#if defined(__ANDROID__)  // Android: 720p or 1440p only (menu value 0 / 1 = choice 1 / 2)
    {kIdResolution, "$lr_res", "$lr_resh", nullptr, 2,
     {"$lr_res_1", "$lr_res_2"}, {"$lr_res_1h", "$lr_res_2h"}},
#else
    {kIdResolution, "$lr_res", "$lr_resh", nullptr, 4,
     {"$lr_res_0", "$lr_res_1", "$lr_res_2", "$lr_res_3"},
     {"$lr_res_0h", "$lr_res_1h", "$lr_res_2h", "$lr_res_3h"}},
#endif
    {kIdAa, "$lr_aa", "$lr_aah", nullptr, 3,
     {"$lr_aa_0", "$lr_aa_1", "$lr_aa_2"}, {"$lr_aa_0h", "$lr_aa_1h", "$lr_aa_2h"}},
    {kIdCheats, "$lr_cht", "$lr_chth", nullptr, 4,
     {"$lr_cht_0", "$lr_cht_1", "$lr_cht_2", "$lr_cht_3"}, {"$lr_cht_0h", "$lr_cht_1h", "$lr_cht_2h", "$lr_cht_3h"}},
#if !defined(__ANDROID__)  // from the Steam version's data (PC only)
    // Controls: Gamepad Controls with a controller connected, Keyboard Settings without one
    // (the keyboard only plays then). One row for both: the list holds 21 items at most.
    {kIdKeyboard, "$lr_kbd", "$lr_kbdh", nullptr, 1, {"$lr_none"}, {"$lr_kbdh"}},
#endif
};

// Texts in the game's own text encoding: plain ASCII, "@r" = line break.
// (The game writes "PlayStation\x85n3" for PlayStation(R)3.)
struct LrText {
  const char* key;
  const char* text;
};
constexpr LrText kTexts[] = {
    {"$lr_voice", "Voice Language"},
    {"$lr_voiceh", "Choose the language of the spoken dialogue."},
    {"$lr_voiceu", "The Japanese voice data is not installed."},
    {"$lr_voice_0", "English"},
    {"$lr_voice_0h", "Characters speak in English."},
    {"$lr_voice_1", "Japanese"},
    {"$lr_voice_1h", "Characters speak in Japanese, with the Japanese lip-sync."},
    {"$lr_text", "Text Language"},
    {"$lr_texth", "Choose the language of the on-screen text. A new choice asks to restart the game."},
    {"$lr_textu", "The Japanese text is not installed (LR_JP_Voice.ps1)."},
    {"$lr_text_0", "English"},
    {"$lr_text_0h", "On-screen text in English."},
    {"$lr_text_1", "Japanese"},
    {"$lr_text_1h", "On-screen text in Japanese."},
    {"$lr_ui", "Button Prompts"},
    {"$lr_uih", "Choose which controller's buttons are shown on screen."},
    {"$lr_ui_a", "Auto (Controller)"},
    {"$lr_ui_ah", "Show the buttons of the controller in use: Xbox or PlayStation."},
    {"$lr_ui_0", "Xbox 360"},
    {"$lr_ui_0h", "Show Xbox 360 controller buttons."},
    {"$lr_ui_1", "PlayStation 3"},
    {"$lr_ui_1h", "Show PlayStation 3 controller buttons."},
    {"$lr_cfm", "Confirm Button"},
    {"$lr_cfmh", "Choose which button confirms and which cancels."},
    {"$lr_cfm_0", "A / Cross"},
    {"$lr_cfm_0h", "A (Cross) confirms and B (Circle) cancels, as on the Xbox 360."},
    {"$lr_cfm_1", "B / Circle"},
    {"$lr_cfm_1h", "B (Circle) confirms and A (Cross) cancels, as in the Japanese release."},
    {"$lr_fps", "Frame Rate"},
    {"$lr_fpsh", "Choose the frame rate of the field, battles, and scenes."},
    {"$lr_fps_0", "30 fps"},
    {"$lr_fps_0h", "30 frames per second, as on the Xbox 360."},
    {"$lr_fps_1", "60 fps (gameplay)"},
    {"$lr_fps_1h", "60 fps in the field, battles, and menus; cutscenes stay at 30 fps, as in the PC version."},
    {"$lr_fps_2", "60 fps (all)"},
    {"$lr_fps_2h", "60 frames per second everywhere, cutscenes included. Game speed stays the same."},
    {"$lr_res", "Resolution"},
    {"$lr_res_0", "Auto (Display)"},
    {"$lr_res_1", "720p"},
    {"$lr_res_2", "1440p"},
    {"$lr_res_3", "2160p (4K)"},
    {"$lr_aa", "Anti-Aliasing"},
    {"$lr_aah", "Smooth jagged edges over the whole picture."},
    {"$lr_aa_0", "Off"},
    {"$lr_aa_0h", "No extra smoothing (the game's own 2x MSAA only)."},
    {"$lr_aa_1", "FXAA"},
    {"$lr_aa_1h", "FXAA: smoother edges, slightly softer text."},
    {"$lr_aa_2", "FXAA Extreme"},
    {"$lr_aa_2h", "Stronger FXAA: smoothest edges, softer picture."},
    {"$lr_cht", "Cheats"},
    {"$lr_chth", "Optional changes to the game's rules."},
    {"$lr_cht_0", "Off"},
    {"$lr_cht_0h", "The game as made."},
    {"$lr_cht_1", "Free Teleport and Chronostasis"},
    {"$lr_cht_1h", "Teleport and Chronostasis cost no EP."},
    {"$lr_cht_2", "Freeze Clock"},
    {"$lr_cht_2h", "The in-game clock stands still: no time passes, the days do not run out."},
    {"$lr_cht_3", "Both"},
    {"$lr_cht_3h", "Free Teleport and Chronostasis, and the clock stands still."},
    {"$lr_kbd", "Controls"},
    {"$lr_kbdh", "Gamepad Controls with a controller connected; Keyboard Settings (assign keys) without one."},
    {"$lr_pad", "Gamepad Controls"},
    {"$lr_padh", "Check the control scheme for when using a gamepad controller."},
    {"$lr_none", ""},
    {"$lr_sdel", "\xF1" "B Delete"},  // Save / Load screen button bar (X icon)
    // "$lr_rstq" (the restart question) is built by lr_dynamic_text.
    // "$lr_resh" and "$lr_res_Nh" are built by lr_dynamic_text (they name the sizes).
};

uint32_t ld32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

void st32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

bool lr_items_enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("LR_CONFIG_MENU_ITEMS");
    return !(v && v[0] == '0');
  }();
  return enabled;
}

const LrItem* lr_item(uint32_t id) {
  for (const LrItem& item : kItems)
    if (item.id == id) return &item;
  return nullptr;
}

// Restart question after a new Resolution (see "Restart question" below): the scale to name,
// set when the choice is stored; the menu whose Yes/No dialog is ours while it is open.
std::atomic<int> g_lr_restart_ask{0};
std::atomic<int> g_lr_restart_scale{0};
std::atomic<uint32_t> g_lr_restart_menu{0};

// Help texts that depend on the display and on what the renderer uses now. The returned
// pointer stays valid until the next call (the text hook copies it right away).
uint8_t* g_lr_menu_base = nullptr;  // guest memory, for texts that read the menu
const char* lr_dynamic_text(const char* key) {
  static std::string text;
  if (std::strncmp(key, "$lr_kb", 6) == 0) {
    if (std::strncmp(key, "$lr_kbhelp", 10) == 0) return LrKeyboardHelp();
    if ((key[6] == 'L' || key[6] == 'R') && g_lr_menu_base) {
      std::string label, value;
      uint32_t row = uint32_t(std::atoi(key + 7));
      if (LrKeyboardRowTexts(g_lr_menu_base, 0x70 + row, label, value)) {
        text = key[6] == 'L' ? label : value;
        return text.c_str();
      }
    }
  }
  auto size = [](int scale) {
    return scale ? std::to_string(1280 * scale) + "x" + std::to_string(720 * scale) : std::string("the config file's size");
  };
  if (std::strcmp(key, "$lr_resh") == 0) {
    int now = LrResolutionActiveScale();
    text = "Render size, now " + size(now) + ". A new choice asks to restart the game.";
    return text.c_str();
  }
  if (std::strcmp(key, "$lr_rstq") == 0 && g_lr_restart_scale.load() < 0) {  // Text Language changed
    text = std::string("Restart the game now to show the text in ") + (LrConfigGetText() ? "Japanese" : "English") +
           "?@rUnsaved progress is lost. Otherwise it changes after returning to the title screen.";
    return text.c_str();
  }
  if (std::strcmp(key, "$lr_rstq") == 0) {
    text = "Restart the game now to render at " + size(g_lr_restart_scale.load()) +
           "?@rUnsaved progress is lost. Otherwise the new size applies at the next start.";
    return text.c_str();
  }
  if (std::strncmp(key, "$lr_res_", 8) == 0 && key[8] >= '0' && key[8] <= '3' && key[9] == 'h' && !key[10]) {
    switch (key[8] - '0') {
      case 0: text = "Match the display: " + size(LrResolutionScaleFor(0)) + " on this PC."; break;
      case 1: text = "1280x720, as on the Xbox 360."; break;
      case 2: text = "2560x1440: sharp on 1080p and 1440p displays."; break;
      default: text = "3840x2160 for 4K displays; needs a fast graphics card."; break;
    }
    return text.c_str();
  }
  return nullptr;
}

const char* lr_text(const char* key) {
  for (const LrText& t : kTexts)
    if (std::strcmp(t.key, key) == 0) return t.text;
  return lr_dynamic_text(key);
}

bool lr_item_selectable(uint32_t id) {
  if (id == kIdText) return LrConfigTextAvailable() != 0;
  return id != kIdVoice || LrConfigVoiceAvailable() != 0;
}

// Shown value. Without the Japanese voice data the voices are English whatever is stored.
int lr_item_value(uint32_t id) {
  if (id == kIdVoice) return (LrConfigGetVoice() && lr_item_selectable(id)) ? 1 : 0;
  if (id == kIdText) return (LrConfigGetText() && lr_item_selectable(id)) ? 1 : 0;
  if (id == kIdFps) return LrConfigGetFps60();
  if (id == kIdAa) return LrConfigGetAa();
  if (id == kIdCheats) return LrConfigGetCheats();
  if (id == kIdConfirm) return LrConfigGetConfirm();
#if defined(__ANDROID__)
  if (id == kIdResolution) return LrConfigGetResolution() >= 2 ? 1 : 0;
#endif
  if (id == kIdResolution) return LrConfigGetResolution();
  if (id == kIdKeyboard || id == kIdGamepad) return 0;
  return LrConfigGetUiMode();
}

uint32_t lr_value_index(const LrItem& item, uint32_t value) {
  return value < item.count ? value : item.count - 1;
}

// Scratch space on the guest stack for the duration of a hook: strings for the game's
// functions, then a frame for the callees (their register save area and the parameter
// area they may write in their caller's frame). r1 is restored on scope exit.
class GuestFrame {
 public:
  GuestFrame(PPCContext& ctx, uint8_t* base)
      : ctx_(ctx), base_(base), saved_r1_(ctx.r1.u32), saved_lr_(ctx.lr) {}
  ~GuestFrame() {
    ctx_.r1.u32 = saved_r1_;
    ctx_.lr = saved_lr_;
  }
  GuestFrame(const GuestFrame&) = delete;
  GuestFrame& operator=(const GuestFrame&) = delete;

  uint32_t PushString(const char* s) {
    uint32_t n = static_cast<uint32_t>(std::strlen(s)) + 1;
    ctx_.r1.u32 = (ctx_.r1.u32 - n) & ~15u;
    std::memcpy(base_ + ctx_.r1.u32, s, n);
    return ctx_.r1.u32;
  }
  uint32_t PushBytes(uint32_t n) {
    ctx_.r1.u32 = (ctx_.r1.u32 - n) & ~15u;
    std::memset(base_ + ctx_.r1.u32, 0, n);
    return ctx_.r1.u32;
  }
  // Call after the last PushString, before the first call into the game.
  void Open() {
    uint32_t sp = (ctx_.r1.u32 - 0x100) & ~15u;
    st32(base_, sp, saved_r1_);  // back chain, as stwu would leave it
    ctx_.r1.u32 = sp;
  }

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t saved_r1_;
  uint64_t saved_lr_;
};

// Value popup being built on this thread (sub_8278C9F8), 0 = none; t_popup_as_id0: built
// as the game's id 0 (its value texts are numbered from kId0ValueTexts).
thread_local uint32_t t_popup_id = 0;
thread_local bool t_popup_as_id0 = false;
// The game id a 3-value popup is built as (its texts are numbered from that id's base).
constexpr uint32_t kThreeValueId = 12;
thread_local uint32_t t_popup_build_id = 0;

std::atomic<bool> g_logged{false};

}  // namespace

REX_EXTERN(sub_827223F0);  // game (GUI) heap allocate(size)
REX_EXTERN(sub_8270C488);  // help bar: (gui, key, 0, 0)
REX_EXTERN(sub_8278BF08);  // row label: (row, key)
REX_EXTERN(sub_8278BF80);  // row value: (row, key)
REX_EXTERN(sub_8278EA00);  // row enabled: (row, 1 = normal, 0 = greyed)
REX_EXTERN(sub_82715CD0);  // play menu sound: (gui, id)
REX_EXTERN(sub_8270D190);  // (gui) -> button guide window or 0
REX_EXTERN(sub_827A1350);  // (button guide window) refresh, as the game does after a sound

namespace {

void lr_show_help(PPCContext& ctx, uint8_t* base, const char* key) {
  GuestFrame frame(ctx, base);
  uint32_t k = frame.PushString(key);
  frame.Open();
  ctx.r3.u64 = ld32(base, kGuiManager);
  ctx.r4.u64 = k;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  sub_8270C488(ctx, base);
}

// row->vtable[0x18](row, hide) (sub_82308428 for the config rows: bit 0x2 of +0xBC = hidden).
void lr_show_row(PPCContext& ctx, uint8_t* base, uint32_t row) {
  uint32_t target = ld32(base, ld32(base, row) + 0x18);
  if (PPCFunc* fn = rex::runtime::ResolveIndirectFunction(target)) {
    ctx.r3.u64 = row;
    ctx.r4.u64 = 0;
    fn(ctx, base);
  } else {
    st32(base, row + 0xBC, ld32(base, row + 0xBC) & ~2u);
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Text: the game asks sub_8270CA90(gui, key) first for every '$' key; answer for ours with
// a heap copy, as the text resource does (double NUL, the game's text terminator).
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8270CA90);
REX_HOOK_RAW(sub_8270CA90) {
  uint32_t key_address = ctx.r4.u32;
  if (key_address) {
    const char* key = reinterpret_cast<const char*>(base + key_address);
    if (key[0] == '$' && key[1] == 'l' && key[2] == 'r' && key[3] == '_') {
      if (const char* text = lr_text(key)) {
        uint32_t len = static_cast<uint32_t>(std::strlen(text));
        auto lr = ctx.lr;
        ctx.r3.u64 = len + 2;
        sub_827223F0(ctx, base);
        ctx.lr = lr;
        uint32_t copy = ctx.r3.u32;
        if (copy) {
          std::memcpy(base + copy, text, len);
          base[copy + len] = 0;
          base[copy + len + 1] = 0;
        }
        ctx.r3.u64 = copy;
        return;
      }
    }
  }
  char ow_key[32];
  bool ow_text = key_address && lr_guest_cstr(base, key_address, ow_key, sizeof(ow_key));
  __imp__sub_8270CA90(ctx, base);
  if (ow_text) lr_cut_outerworld_text(base, ow_key, ctx.r3.u32);  // Outerworld stripped
  lr_title_quit_label(base, ctx.r3.u32);                           // title: QUIT TO DESKTOP
}

// ---------------------------------------------------------------------------------------
// Item list: after the game has picked its items, put ours in front of the two license
// entries (ids 18 and 19), or at the end. The values come from the backend each time the
// menu opens.
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8278D648);
REX_HOOK_RAW(sub_8278D648) {
  uint32_t list = ctx.r3.u32;
  __imp__sub_8278D648(ctx, base);
  // Outerworld is stripped (see lr_hooks.cpp): "Outerworld Services" (id 10), "Outerworld
  // Characters" (12), "Link with Facebook" (13), and "Link with Twitter" (14) are not listed.
  if (!std::getenv("LR_OUTERWORLD_PROMPT")) {
    uint32_t count = ld32(base, list + kListCount), kept = 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t id = ld32(base, list + kListIds + 4 * i);
      if (id == 10 || id == 12 || id == 13 || id == 14) continue;
      st32(base, list + kListIds + 4 * kept, id);
      st32(base, list + kListValues + 4 * kept, ld32(base, list + kListValues + 4 * i));
      ++kept;
    }
    if (kept != count) {
      st32(base, list + kListCount, kept);
      static std::atomic<bool> ow_logged{false};
      if (!ow_logged.exchange(true))
        REXLOG_INFO("LR Outerworld: removed ({} Settings items: Outerworld Services, Outerworld "
                    "Characters, Link with Facebook, Link with Twitter)", count - kept);
    }
  }
  if (!lr_items_enabled()) return;
  constexpr uint32_t n = sizeof(kItems) / sizeof(kItems[0]);
  uint32_t count = ld32(base, list + kListCount);
  if (count >= kListCapacity) {
    REXLOG_WARN("LR config menu: {} items, no room for the extra ones", count);
    return;
  }
  // With more game items than usual, the last of ours are left out.
  const uint32_t added = count + n > kListCapacity ? kListCapacity - count : n;
  uint32_t pos = count;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t id = ld32(base, list + kListIds + 4 * i);
    if (id == 18 || id == 19) {
      pos = i;
      break;
    }
  }
  for (uint32_t i = count; i-- > pos;) {
    st32(base, list + kListIds + 4 * (i + added), ld32(base, list + kListIds + 4 * i));
    st32(base, list + kListValues + 4 * (i + added), ld32(base, list + kListValues + 4 * i));
  }
  for (uint32_t k = 0; k < added; ++k) {
    st32(base, list + kListIds + 4 * (pos + k), kItems[k].id);
    st32(base, list + kListValues + 4 * (pos + k), lr_item_value(kItems[k].id));
  }
  st32(base, list + kListCount, count + added);
  ctx.r3.u64 = list;
  if (!g_logged.exchange(true))
    REXLOG_INFO("LR config menu: {} of {} items added (Voice Language, Text Language, Button Prompts, Confirm Button, "
                "Frame Rate, Resolution, Anti-Aliasing, Cheats, Controls) at {} of {}",
                added, n, pos, count + added);
}

// ---------------------------------------------------------------------------------------
// Row fill (every row refresh: open, scroll, value change, restore defaults).
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8278EEB0);
void lr_kb_align(uint8_t* base, uint32_t row);
REX_HOOK_RAW(sub_8278EEB0) {
  uint32_t list = ctx.r3.u32, row = ctx.r4.u32, index = ctx.r5.u32;
  g_lr_menu_base = base;
  if (index < ld32(base, list + kListCount)) {
    uint32_t id = ld32(base, list + kListIds + 4 * index);
    if (id >= 0x70 && id < 0x7C) {
      std::string l = "$lr_kbL" + std::to_string(id - 0x70), r = "$lr_kbR" + std::to_string(id - 0x70);
      GuestFrame frame(ctx, base);
      uint32_t label = frame.PushString(l.c_str());
      uint32_t text = frame.PushString(r.c_str());
      frame.Open();
      lr_show_row(ctx, base, row);
      ctx.r3.u64 = row;
      ctx.r4.u64 = label;
      sub_8278BF08(ctx, base);
      ctx.r3.u64 = row;
      ctx.r4.u64 = text;
      sub_8278BF80(ctx, base);
      ctx.r3.u64 = row;
      ctx.r4.u64 = 1;
      sub_8278EA00(ctx, base);
      lr_kb_align(base, row);
      return;
    }
  }
  const LrItem* item = nullptr;
  if (index < ld32(base, list + kListCount)) item = lr_item(ld32(base, list + kListIds + 4 * index));
  if (!item) {
    __imp__sub_8278EEB0(ctx, base);
    return;
  }
  uint32_t value = lr_value_index(*item, ld32(base, list + kListValues + 4 * index));
  GuestFrame frame(ctx, base);
  uint32_t label = frame.PushString(item->label);
  uint32_t text = frame.PushString(item->value[value]);
  frame.Open();
  lr_show_row(ctx, base, row);
  ctx.r3.u64 = row;
  ctx.r4.u64 = label;
  sub_8278BF08(ctx, base);
  ctx.r3.u64 = row;
  ctx.r4.u64 = text;
  sub_8278BF80(ctx, base);
  ctx.r3.u64 = row;
  ctx.r4.u64 = lr_item_selectable(item->id) ? 1 : 0;
  sub_8278EA00(ctx, base);
}

// ---------------------------------------------------------------------------------------
// Help bar for the item under the cursor (MainConfigMenu vtable +0x15C).
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8278DC38);
REX_HOOK_RAW(sub_8278DC38) {
  if (LrKeyboardViewOpen(ctx.r3.u32)) {
    // Two keys in turn: the help bar skips a key it already shows, and our text changes.
    static bool flip = false;
    flip = !flip;
    lr_show_help(ctx, base, flip ? "$lr_kbhelp" : "$lr_kbhelp2");
    return;
  }
  uint32_t list = ld32(base, ctx.r3.u32 + kMenuList);
  if (list) {
    uint32_t cursor = ld32(base, list + kConfigListCursor);
    if (cursor < ld32(base, list + kConfigListCount)) {
      if (const LrItem* item = lr_item(ld32(base, list + kConfigListItems + 4 * cursor))) {
        const char* key = item->help;
        if (!lr_item_selectable(item->id) && item->help_unavailable) key = item->help_unavailable;
        lr_show_help(ctx, base, key);
        return;
      }
    }
  }
  __imp__sub_8278DC38(ctx, base);
}

// ---------------------------------------------------------------------------------------
// A on a greyed item: the game's own refusal (buzzer), as for its disabled items.
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8278EB38);
REX_HOOK_RAW(sub_8278EB38) {
  uint32_t menu = ctx.r3.u32;
  uint32_t list = ld32(base, menu + kMenuList);
  // Same preconditions as the game: no dialog/popup open, list not scrolling.
  if (list && !ld32(base, menu + 0xB8) && !ld32(base, menu + 0xBC) && !ld32(base, menu + 0xB0) &&
      !ld32(base, list + kConfigListScrolling)) {
    uint32_t cursor = ld32(base, list + kConfigListCursor);
    if (cursor < ld32(base, list + kConfigListCount)) {
      uint32_t id = ld32(base, list + kConfigListItems + 4 * cursor);
      if (id == kIdKeyboard && !g_lr_keyboard_mode.load()) id = kIdGamepad;  // a controller is connected
      if (LrKeyboardActivate(ctx, base, menu, id)) return;
      if (id == kIdGamepad) {
        const bool opened = LrControlsOpen();
        GuestFrame frame(ctx, base);
        frame.Open();
        ctx.r3.u64 = ld32(base, kGuiManager);
        ctx.r4.u64 = opened ? 4 : 0x49;  // confirm, or the buzzer without the picture
        sub_82715CD0(ctx, base);
        return;
      }
      if (lr_item(id) && !lr_item_selectable(id)) {
        GuestFrame frame(ctx, base);
        frame.Open();
        uint32_t gui = ld32(base, kGuiManager);
        ctx.r3.u64 = gui;
        ctx.r4.u64 = kSoundBuzzer;
        sub_82715CD0(ctx, base);
        ctx.r3.u64 = gui;
        sub_8270D190(ctx, base);
        if (ctx.r3.u32) sub_827A1350(ctx, base);
        return;
      }
    }
  }
  __imp__sub_8278EB38(ctx, base);
}

// ---------------------------------------------------------------------------------------
// Value popup: the game builds it (2 values for any id but 0 and 12, 4 for id 0); only the
// value texts need our keys. sub_8278AA78(buf, size, number) is asked for "$f_con_%02d" with
// number = base[id] + 1 + value (id 0: kId0ValueTexts[value]) while the popup is built. A
// 4-value item is built as id 0 and gets its own id back before anything reads it.
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8278C9F8);
REX_HOOK_RAW(sub_8278C9F8) {
  const LrItem* item = lr_item(ctx.r4.u32);
  if (!item) {
    __imp__sub_8278C9F8(ctx, base);
    return;
  }
  uint32_t popup = ctx.r3.u32;
  uint32_t saved = t_popup_id, saved_build_id = t_popup_build_id;
  bool saved_as_id0 = t_popup_as_id0;
  t_popup_id = item->id;
  t_popup_as_id0 = item->count == 4;
  t_popup_build_id = item->count == 4 ? 0 : item->count == 3 ? kThreeValueId : item->id;
  ctx.r4.u64 = t_popup_build_id;
  __imp__sub_8278C9F8(ctx, base);
  if (t_popup_build_id != item->id) {
    st32(base, popup + kPopupId, item->id);
    st32(base, popup + kPopupId2, item->id);
  }
  t_popup_id = saved;
  t_popup_build_id = saved_build_id;
  t_popup_as_id0 = saved_as_id0;
}

REX_EXTERN(__imp__sub_8278AA78);
REX_HOOK_RAW(sub_8278AA78) {
  if (t_popup_id) {
    const LrItem* item = lr_item(t_popup_id);
    uint32_t value = ~0u;
    if (t_popup_as_id0) {
      for (uint32_t k = 0; k < 4; ++k)
        if (ld32(base, kId0ValueTexts + 4 * k) == ctx.r5.u32) value = k;
    } else {
      value = ctx.r5.u32 - (ld32(base, kBaseTextTable + 4 * t_popup_build_id) + 1);
    }
    uint32_t buf = ctx.r3.u32, size = ctx.r4.u32;
    if (item && value < item->count && buf && size) {
      const char* key = item->value[value];
      uint32_t n = static_cast<uint32_t>(std::strlen(key));
      if (n >= size) n = size - 1;
      std::memcpy(base + buf, key, n);
      base[buf + n] = 0;
      ctx.r3.u64 = buf;
      return;
    }
  }
  __imp__sub_8278AA78(ctx, base);
}

// Help bar while the popup is open (popup vtable +0x15C), for the hovered value.
REX_EXTERN(__imp__sub_8278B4D0);
REX_HOOK_RAW(sub_8278B4D0) {
  uint32_t popup = ctx.r3.u32;
  if (const LrItem* item = lr_item(ld32(base, popup + kPopupId))) {
    lr_show_help(ctx, base, item->value_help[lr_value_index(*item, ld32(base, popup + kPopupHovered))]);
    return;
  }
  __imp__sub_8278B4D0(ctx, base);
}

// ---------------------------------------------------------------------------------------
// Store / apply / default. The game calls the store also when the popup is cancelled (with
// the old value), so the backend is only told about real changes.
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_8272FA78);
REX_HOOK_RAW(sub_8272FA78) {
  uint32_t id = ctx.r3.u32;
  int value = ctx.r4.u32 ? 1 : 0;
  if (id == kIdVoice) {
    if (value != (LrConfigGetVoice() ? 1 : 0)) LrConfigSetVoice(value);
    return;
  }
  if (id == kIdText) {
    if (value != (LrConfigGetText() ? 1 : 0)) {
      LrConfigSetText(value);
      g_lr_restart_ask.store(-1);  // the menus' text is loaded at the start: ask to restart
    }
    return;
  }
  if (id == kIdUiStyle) {
    int mode = int(ctx.r4.u32 < 3 ? ctx.r4.u32 : 0);
    if (mode != LrConfigGetUiMode()) LrConfigSetUiMode(mode);
    return;
  }
  if (id == kIdConfirm) {
    if (value != LrConfigGetConfirm()) LrConfigSetConfirm(value);
    return;
  }
  if (id == kIdFps) {
    int choice = int(ctx.r4.u32 < 3 ? ctx.r4.u32 : 0);
    if (choice != LrConfigGetFps60()) LrConfigSetFps60(choice);
    return;
  }
  if (id == kIdAa) {
    int choice = int(ctx.r4.u32 < 3 ? ctx.r4.u32 : 0);
    if (choice != LrConfigGetAa()) LrConfigSetAa(choice);
    return;
  }
  if (id == kIdCheats) {
    int choice = int(ctx.r4.u32 < 4 ? ctx.r4.u32 : 0);
    if (choice != LrConfigGetCheats()) LrConfigSetCheats(choice);
    return;
  }
  if (id == kIdResolution) {
    int choice = int(ctx.r4.u32 < 4 ? ctx.r4.u32 : 0);
#if defined(__ANDROID__)
    choice = ctx.r4.u32 ? 2 : 1;
#endif
    if (choice != LrConfigGetResolution()) {
      if (LrConfigSetResolution(choice)) g_lr_restart_ask.store(LrResolutionScaleFor(choice));
    } else {
      REXLOG_INFO("LR resolution: Settings closed, choice unchanged ({})", choice);
    }
    return;
  }
  __imp__sub_8272FA78(ctx, base);
}

REX_EXTERN(__imp__sub_8272F8C0);
REX_HOOK_RAW(sub_8272F8C0) {
  if (lr_item(ctx.r3.u32)) return;
  __imp__sub_8272F8C0(ctx, base);
}

REX_EXTERN(__imp__sub_8272F598);
REX_HOOK_RAW(sub_8272F598) {
  uint32_t id = ctx.r3.u32;
  if (id == kIdVoice) {
    ctx.r3.u64 = (kDefaultVoiceJp && lr_item_selectable(kIdVoice)) ? 1 : 0;
    return;
  }
  if (id == kIdUiStyle) {
    ctx.r3.u64 = kDefaultUiMode;
    return;
  }
  if (id == kIdText || id == kIdAa || id == kIdCheats) {
    ctx.r3.u64 = 0;
    return;
  }
  if (id == kIdResolution) {
    ctx.r3.u64 = kDefaultResolution;
    return;
  }
  if (id == kIdFps) {
    ctx.r3.u64 = kDefaultFps60;
    return;
  }
  if (id == kIdConfirm) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8272F598(ctx, base);
}

// ---------------------------------------------------------------------------------------
// Restart question. A new Resolution needs a restart (the renderer's scale is fixed while it
// runs: rebuilding it live lost the menus and fonts the game had built on the GPU). The
// question is the menu's own Yes/No dialog, the one "Restore defaults" opens (sub_8278DB30:
// sub_8278C430(menu, "question|choices"), kept at +0xA8): driven by the pad, same look and
// sounds. The step (sub_8278F7F0) closes it and calls sub_8278F4A8 for the first choice
// (Yes); for our dialog that restarts the game instead of restoring the defaults. No keeps
// the choice for the next start.
// ---------------------------------------------------------------------------------------
REX_EXTERN(sub_82728E20);  // LocalizedText ctor (text)
REX_EXTERN(sub_82729450);  // LocalizedText::Set (text, key, 0)
REX_EXTERN(sub_82728E38);  // LocalizedText dtor (text)
REX_EXTERN(sub_8278C430);  // config menu: open its Yes/No dialog (menu, "question|choices")

namespace {
constexpr uint32_t kMenuPopup = 0xA0, kMenuDialog = 0xA8, kMenuBusyB0 = 0xB0, kMenuBusyB8 = 0xB8,
                   kMenuBusyBC = 0xBC;
constexpr uint32_t kSoundDialog = 0x4A;  // what sub_8278DB30 plays when it opens the dialog

// A key's text as the game resolves it (our "$lr_" keys come from the text hook above).
std::string lr_resolved_text(PPCContext& ctx, uint8_t* base, const char* key) {
  GuestFrame frame(ctx, base);
  uint32_t k = frame.PushString(key);
  uint32_t text = frame.PushBytes(64);
  frame.Open();
  ctx.r3.u64 = text;
  sub_82728E20(ctx, base);
  ctx.r3.u64 = text;
  ctx.r4.u64 = k;
  ctx.r5.u64 = 0;
  sub_82729450(ctx, base);
  std::string out;
  uint32_t chars = ld32(base, text);
  if (chars) {
    uint16_t offset_be;
    std::memcpy(&offset_be, base + text + 10, 2);
    chars += uint32_t(int32_t(int16_t(__builtin_bswap16(offset_be))));
    for (uint32_t i = 0; i < 480 && base[chars + i]; ++i) out += char(base[chars + i]);
  }
  ctx.r3.u64 = text;
  sub_82728E38(ctx, base);
  return out;
}

bool lr_menu_idle(uint8_t* base, uint32_t menu) {
  return !ld32(base, menu + kMenuPopup) && !ld32(base, menu + kMenuDialog) &&
         !ld32(base, menu + kMenuBusyB0) && !ld32(base, menu + kMenuBusyB8) &&
         !ld32(base, menu + kMenuBusyBC);
}

void lr_open_restart_dialog(PPCContext& ctx, uint8_t* base, uint32_t menu) {
  std::string question = lr_resolved_text(ctx, base, "$lr_rstq");
  std::string choices = lr_resolved_text(ctx, base, "$f_con_def_01");  // the game's Yes / No
  REXLOG_INFO("LR resolution: asking to restart ('{}' | '{}')", question, choices);
  std::string all = question + "|" + choices;
  GuestFrame frame(ctx, base);
  uint32_t text = frame.PushString(all.c_str());
  frame.Open();
  ctx.r3.u64 = menu;
  ctx.r4.u64 = text;
  sub_8278C430(ctx, base);
  ctx.r3.u64 = ld32(base, kGuiManager);
  ctx.r4.u64 = kSoundDialog;
  sub_82715CD0(ctx, base);
}
}  // namespace

REX_EXTERN(__imp__sub_8278F7F0);
REX_HOOK_RAW(sub_8278F7F0) {
  uint32_t menu = ctx.r3.u32;
  if (LrKeyboardViewOpen(menu)) {
    PPCContext saved = ctx;
    LrKeyboardStep(ctx, base, menu);
    ctx = saved;
  }
  if (g_lr_restart_ask.load() && lr_menu_idle(base, menu)) {
    g_lr_restart_scale.store(g_lr_restart_ask.exchange(0));
    double step = ctx.f1.f64;
    lr_open_restart_dialog(ctx, base, menu);
    if (ld32(base, menu + kMenuDialog)) {
      g_lr_restart_menu.store(menu);
    } else {
      REXLOG_WARN("LR resolution: the restart question did not open; the choice applies at the next start");
    }
    ctx.r3.u64 = menu;
    ctx.f1.f64 = step;
  }
  __imp__sub_8278F7F0(ctx, base);
  if (g_lr_restart_menu.load() == menu && !ld32(base, menu + kMenuDialog)) {
    g_lr_restart_menu.store(0);
    REXLOG_INFO("LR resolution: restart declined; the new size applies at the next start");
  }
}

// Restore defaults - or Yes in our restart question.
REX_EXTERN(__imp__sub_8278F4A8);
REX_HOOK_RAW(sub_8278F4A8) {
  if (LrKeyboardDialogYes(ctx, base, ctx.r3.u32)) return;
  if (g_lr_restart_menu.load() == ctx.r3.u32) {
    g_lr_restart_menu.store(0);
    LrResolutionRestartNow();
    return;  // could not restart: the choice applies at the next start
  }
  __imp__sub_8278F4A8(ctx, base);
}


// ---------------------------------------------------------------------------------------
// Deleting a save in the game's own Save / Load screen (#SaveLoadX360, GuiSaveLoadX360): X on
// a save asks "$save_del_xmb00" ("Do you wish to delete save data?", an unused PS3 text). Yes
// closes the list the way Y (select storage device) does, so the save worker (sub_82658A90)
// forgets the device, enumerates again, and reopens the list; the delete itself runs on that
// worker right before it enumerates (sub_82658908), because while the list is open the worker
// keeps mounting the saves near the cursor and an open save cannot be deleted.
//   menu (vtable 0x8212AA50): +0x54 product id, +0x110 state (2 = idle), +0x114 hovered display
//   index, +0x11C display count, +0x558 autosave hidden, +0x55C its entry index.
//   saves: S = [[0x82F62030]+0x46C], enumerator [S+0x178]: +8 entries of 0x180 bytes (an
//   XCONTENT_DATA first: file name at +0x108; slot number s8 at +0x14A), +0x14 user, +0x18
//   count (s8, the new-save entry not counted). Slot 50 is the autosave, which the worker makes
//   again when it is missing: it is not deleted.
//   input: sub_827E7AB8(delegate, event, product), pressed bits [event+0xC]; X = 0x40.
//   question: sub_827153A0(gui, "#SaveLoadMsg", 0,0,0,0, "key|$theme_sel_yes|$theme_sel_no", 1,
//   f1 0, zero stack words), sub_8270E198(gui, name, 1) = window, sub_827CE6C8(w, 1, 1),
//   w->vtbl[0x13C](w, 0, 1); answered when sub_827CBA80(w, 1) != 0, [w+0xA0] == 0 = Yes.
//   button bar: built by sub_8273F048(text, key) calls; the one returning to 0x827E6798 adds
//   "A OK", "X Delete" goes in front of it.
// ---------------------------------------------------------------------------------------
REX_EXTERN(__imp__sub_827E7AB8);
REX_EXTERN(__imp__sub_827E7278);
REX_EXTERN(__imp__sub_82658908);
REX_EXTERN(__imp__sub_82658A90);
REX_EXTERN(__imp__sub_8273F048);
REX_EXTERN(__imp__sub_827E3E40);
REX_EXTERN(sub_827153A0);  // message window: create
REX_EXTERN(sub_8270E198);  // find a product by name (gui, name, lock)
REX_EXTERN(sub_827CE6C8);
REX_EXTERN(sub_827CBA80);  // message window: answered?
REX_EXTERN(sub_8270FCA8);  // close a product (gui, id, 0)
REX_EXTERN(sub_82D32300);  // XamContentDelete (user, XCONTENT_DATA*, overlapped)

namespace {
constexpr uint32_t kSaveLoadVtable = 0x8212AA50;
constexpr uint32_t kSaveLoadMsg = 0x83127E30, kSaveLoadMsg2 = 0x83127E20, kSaveLoadMsg3 = 0x83127E48;
constexpr uint32_t kBarOkReturn = 0x827E6798;
constexpr uint32_t kPadX = 0x40;
constexpr int kAutosaveSlot = 50;
enum : int { kDelIdle, kDelAsking, kDelPending };
std::atomic<int> g_del_state{kDelIdle};
std::atomic<uint32_t> g_del_menu{0};
std::atomic<int> g_del_cursor{-1};  // display index to put the cursor on after the list reopens
int g_del_disp = -1;
uint8_t g_del_content[0x134];  // XCONTENT_DATA of the save to delete (guest byte order)

uint32_t lr_save_enumerator(uint8_t* base) {
  uint32_t app = ld32(base, 0x82F62030);
  uint32_t s = app ? ld32(base, app + 0x46C) : 0;
  return s ? ld32(base, s + 0x178) : 0;
}

// Calls below run on a copy of the hook's context, inside a GuestFrame of that copy.
uint32_t lr_find_product(PPCContext& c, uint8_t* base, uint32_t name) {
  c.r3.u64 = ld32(base, kGuiManager);
  c.r4.u64 = name;
  c.r5.u64 = 1;
  sub_8270E198(c, base);
  return c.r3.u32;
}

void lr_menu_sound(PPCContext& c, uint8_t* base, uint32_t id) {
  c.r3.u64 = ld32(base, kGuiManager);
  c.r4.u64 = id;
  sub_82715CD0(c, base);
}

bool lr_open_delete_question(PPCContext& ctx, uint8_t* base) {
  PPCContext c = ctx;
  GuestFrame frame(c, base);
  uint32_t text = frame.PushString("$save_del_xmb00|$theme_sel_yes|$theme_sel_no");
  frame.Open();
  std::memset(base + c.r1.u32 + 8, 0, 0xF8);  // the callee's stack arguments: all 0
  c.r3.u64 = ld32(base, kGuiManager);
  c.r4.u64 = kSaveLoadMsg;
  c.r5.u64 = c.r6.u64 = c.r7.u64 = c.r8.u64 = 0;
  c.r9.u64 = text;
  c.r10.u64 = 1;
  c.f1.f64 = 0.0;
  sub_827153A0(c, base);
  uint32_t w = lr_find_product(c, base, kSaveLoadMsg);
  if (!w) return false;
  c.r3.u64 = w;
  c.r4.u64 = 1;
  c.r5.u64 = 1;
  sub_827CE6C8(c, base);
  uint32_t open = ld32(base, ld32(base, w) + 0x13C);
  c.r3.u64 = w;
  c.r4.u64 = 0;
  c.r5.u64 = 1;
  rex::runtime::ResolveIndirectFunction(open)(c, base);
  return true;
}
}  // namespace

// Input: X on a save asks whether to delete it.
REX_HOOK_RAW(sub_827E7AB8) {
  uint32_t ev = ctx.r4.u32, menu = ctx.r5.u32;
  if (!menu || ld32(base, menu) != kSaveLoadVtable) {
    __imp__sub_827E7AB8(ctx, base);
    return;
  }
  if (g_del_state.load() == kDelAsking && g_del_menu.load() == menu) return;  // our question is open
  if (!(ld32(base, ev + 0xC) & kPadX) || ld32(base, menu + 0x110) != 2 || g_del_state.load() != kDelIdle) {
    __imp__sub_827E7AB8(ctx, base);
    return;
  }
  PPCContext c = ctx;
  {
    GuestFrame frame(c, base);
    frame.Open();
    if (lr_find_product(c, base, kSaveLoadMsg) || lr_find_product(c, base, kSaveLoadMsg2) ||
        lr_find_product(c, base, kSaveLoadMsg3))
      return;
    int disp = int32_t(ld32(base, menu + 0x114)), count = int32_t(ld32(base, menu + 0x11C));
    int entry = (ld32(base, menu + 0x558) && disp >= int32_t(ld32(base, menu + 0x55C))) ? disp + 1 : disp;
    uint32_t en = lr_save_enumerator(base);
    int saves = en ? int8_t(base[en + 0x18]) : 0;
    uint32_t e = en ? ld32(base, en + 8) + uint32_t(entry) * 0x180 : 0;
    int slot = e && entry >= 0 && entry < saves ? int8_t(base[e + 0x14A]) : -1;
    if (disp < 0 || disp >= count || slot < 0 || slot == kAutosaveSlot) {  // new-save entry, autosave
      lr_menu_sound(c, base, kSoundBuzzer);
      return;
    }
    std::memcpy(g_del_content, base + e, sizeof(g_del_content));
    g_del_disp = disp;
    g_del_menu.store(menu);
    g_del_state.store(kDelAsking);
    lr_menu_sound(c, base, 4);
  }
  if (!lr_open_delete_question(ctx, base)) {
    g_del_state.store(kDelIdle);
    REXLOG_WARN("LR saves: the delete question did not open");
    return;
  }
  REXLOG_INFO("LR saves: delete {}? (asked)", reinterpret_cast<const char*>(g_del_content + 0x108));
}

// Step: the answer. Yes closes the list as Y does (the worker deletes, then lists again).
REX_HOOK_RAW(sub_827E7278) {
  uint32_t menu = ctx.r3.u32;
  if (g_del_state.load() == kDelAsking && g_del_menu.load() == menu) {
    PPCContext c = ctx;
    GuestFrame frame(c, base);
    frame.Open();
    uint32_t w = lr_find_product(c, base, kSaveLoadMsg);
    if (!w) {
      g_del_state.store(kDelIdle);
      st32(base, menu + 0x110, 1);
    } else {
      c.r3.u64 = w;
      c.r4.u64 = 1;
      sub_827CBA80(c, base);
      if (c.r3.u32) {
        if (ld32(base, w + 0xA0) == 0) {
          g_del_state.store(kDelPending);
          uint32_t gui = ld32(base, kGuiManager);
          st32(base, gui + 0x1048, 1);
          lr_menu_sound(c, base, 4);
          c.r3.u64 = gui;
          c.r4.u64 = ld32(base, menu + 0x54);
          c.r5.u64 = 0;
          sub_8270FCA8(c, base);
          REXLOG_INFO("LR saves: delete confirmed; the list is read again");
          return;  // the list is closing: no step of it this frame
        } else {
          g_del_state.store(kDelIdle);
          st32(base, menu + 0x110, 1);  // as the game's own "No": focus back to the list
          REXLOG_INFO("LR saves: delete cancelled");
        }
      }
    }
  }
  __imp__sub_827E7278(ctx, base);
}

// Save worker: a new run forgets a delete that never got to its enumeration.
REX_HOOK_RAW(sub_82658A90) {
  if (g_del_state.load() == kDelPending) g_del_state.store(kDelIdle);
  __imp__sub_82658A90(ctx, base);
}

// Save worker, before it enumerates the saves again: the delete.
REX_HOOK_RAW(sub_82658908) {
  if (g_del_state.load() == kDelPending) {
    uint32_t en = ctx.r3.u32;
    PPCContext c = ctx;
    GuestFrame frame(c, base);
    uint32_t content = frame.PushBytes(sizeof(g_del_content));
    std::memcpy(base + content, g_del_content, sizeof(g_del_content));
    frame.Open();
    c.r3.u64 = ld32(base, en + 0x14);
    c.r4.u64 = content;
    c.r5.u64 = 0;
    sub_82D32300(c, base);
    uint32_t result = c.r3.u32;
    REXLOG_INFO("LR saves: {} deleted (result {:X})", reinterpret_cast<const char*>(g_del_content + 0x108), result);
    g_del_cursor.store(g_del_disp);
    g_del_state.store(kDelIdle);
  }
  __imp__sub_82658908(ctx, base);
}

// Cursor of the reopened list: where the deleted save was.
// sub_827E3E40(display count, autosave hidden, autosave index) = the first cursor position.
REX_HOOK_RAW(sub_827E3E40) {
  int cursor = g_del_cursor.exchange(-1);
  int count = int32_t(ctx.r3.u32);
  __imp__sub_827E3E40(ctx, base);
  if (cursor >= 0) ctx.r3.u64 = uint32_t(count > 0 ? std::min(cursor, count - 1) : 0);
}

// Button bar: "X Delete" in front of "A OK".
REX_HOOK_RAW(sub_8273F048) {
  if (uint32_t(ctx.lr) == kBarOkReturn) {
    PPCContext c = ctx;
    GuestFrame frame(c, base);
    uint32_t key = frame.PushString("$lr_sdel");
    frame.Open();
    c.r3.u64 = ctx.r3.u64;
    c.r4.u64 = key;
    __imp__sub_8273F048(c, base);
  }
  __imp__sub_8273F048(ctx, base);
}
