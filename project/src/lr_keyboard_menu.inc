// lightningreturns - Settings > Keyboard Settings and Gamepad Controls (the Steam version's
// screens, PC).
//
// Keyboard Settings is a second view of the game's own Settings list: the same rows, cursor,
// sounds, help bar, and Yes/No dialog. Twelve rows of two commands each, in the Steam version's
// order (left: movement, cursor, camera; right: the buttons), every command followed by its
// button icon - which the key icons (lr_keyboard_icons.inc) turn into the bound key.
//   Up/Down   row               Left/Right  column (the selected command is highlighted)
//   A         press the new key (the help bar asks for it; the game sees no input meanwhile)
//   X         restore this key's default (Yes/No)        B   back to Settings
// A key another command uses asks whether to swap the two (Yes/No, as Steam). Ctrl, Alt, the
// Windows keys, Num Lock, and Scroll Lock cannot be assigned (Steam's rule).
// Bindings are the runtime's keybind cvars and keyboard.txt in the settings folder.

#include <rex/ui/virtual_key.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace {

constexpr uint32_t kIdKeyboardRow = 0x70;  // ids of the twelve rows: 0x70..0x7B
constexpr uint32_t kKeyboardRows = 12;
constexpr uint32_t kSoundConfirm = 4, kSoundCancel = 3, kSoundCursor = 5;

// Commands: text and the button code of their icon (the second byte after 0xF1), per slot.
struct LrKeyCommand {
  const char* text;
  char icon;
  const char* default_key;
};
constexpr LrKeyCommand kKeyCommands[24] = {
    {"Character movement (forward)", 'Q', "W"},
    {"Character movement (backward)", 'O', "S"},
    {"Character movement (left)", 'N', "A"},
    {"Character movement (right)", 'P', "D"},
    {"Cursor movement (up)", 'M', "Up"},
    {"Cursor movement (down)", 'K', "Down"},
    {"Cursor movement (left)", 'J', "Left"},
    {"Cursor movement (right)", 'L', "Right"},
    {"Camera movement (up)", 'Y', "I"},
    {"Camera movement (down)", 'W', "K"},
    {"Camera movement (left)", 'V', "J"},
    {"Camera movement (right)", 'X', "L"},
    {"Confirm selections / activate abilities", '@', "Return"},
    {"Cancel / Jump / activate abilities", 'A', "Backspace"},
    {"Menu/ activate abilities", 'C', "Period"},
    {"Display map / activate abilities", 'B', "M"},
    {"Change between schemata", 'F', "E"},
    {"Attack (start battle)/ Change schemata", 'G', "U"},
    {"Overclock", 'H', "Q"},
    {"Dash (while moving) / Libra (enemy info)", 'I', "O"},
    {"Open the pause menu", 'D', "Escape"},
    {"Switch the navigational map on or off", 'E', "F1"},
    {"Reset map position (while map is open)", 'T', "X"},
    {"Reset camera/ Change camera mode", 'U', "Comma"},
};

// Window key code -> keybind name, for the keys that can be assigned.
const char* lr_key_name(rex::ui::VirtualKey key) {
  using rex::ui::VirtualKey;
  const unsigned vk = unsigned(key);
  if (vk >= unsigned(VirtualKey::kA) && vk <= unsigned(VirtualKey::kZ)) {
    static const char* kLetters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                     "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    return kLetters[vk - unsigned(VirtualKey::kA)];
  }
  if (vk >= unsigned(VirtualKey::k0) && vk <= unsigned(VirtualKey::k9)) {
    static const char* kDigits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    return kDigits[vk - unsigned(VirtualKey::k0)];
  }
  if (vk >= unsigned(VirtualKey::kF1) && vk <= unsigned(VirtualKey::kF12)) {
    static const char* kF[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    return kF[vk - unsigned(VirtualKey::kF1)];
  }
  if (vk >= unsigned(VirtualKey::kNumpad0) && vk <= unsigned(VirtualKey::kNumpad9)) {
    static const char* kNum[] = {"Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4",
                                 "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9"};
    return kNum[vk - unsigned(VirtualKey::kNumpad0)];
  }
  switch (key) {
    case VirtualKey::kReturn: return "Return";  // Enter and the keypad's Enter alike
    case VirtualKey::kEscape: return "Escape";
    case VirtualKey::kBack: return "Backspace";
    case VirtualKey::kTab: return "Tab";
    case VirtualKey::kSpace: return "Space";
    case VirtualKey::kOemMinus: return "Minus";
    case VirtualKey::kOemPlus: return "Plus";
    case VirtualKey::kOem4: return "LBracket";
    case VirtualKey::kOem6: return "RBracket";
    case VirtualKey::kOem5: return "Backslash";
    case VirtualKey::kOem1: return "Semicolon";
    case VirtualKey::kOem7: return "Quote";
    case VirtualKey::kOem3: return "Backtick";
    case VirtualKey::kOemComma: return "Comma";
    case VirtualKey::kOemPeriod: return "Period";
    case VirtualKey::kOem2: return "Slash";
    case VirtualKey::kShift:
    case VirtualKey::kLShift:
    case VirtualKey::kRShift: return "Shift";
    case VirtualKey::kUp: return "Up";
    case VirtualKey::kDown: return "Down";
    case VirtualKey::kLeft: return "Left";
    case VirtualKey::kRight: return "Right";
    case VirtualKey::kHome: return "Home";
    case VirtualKey::kEnd: return "End";
    case VirtualKey::kPrior: return "PageUp";
    case VirtualKey::kNext: return "PageDown";
    case VirtualKey::kInsert: return "Insert";
    case VirtualKey::kDelete: return "Delete";
    default: return nullptr;
  }
}

// Key events as the window delivers them (LrKeyInput, from an input listener the app adds to
// the game window: the same events the keyboard driver gets). An SDL event watch does not work
// on Windows: the runtime DLL has its own copy of SDL, so the exe's copy never sees the window.
struct LrKeyWatch {
  std::mutex mutex;
  bool held[256] = {};
  uint32_t downs = 0;  // key-down count
  int last = 0;        // window key code of the last key down
};
LrKeyWatch& lr_key_watch() {
  static LrKeyWatch watch;
  return watch;
}
bool lr_any_key_held() {
  LrKeyWatch& w = lr_key_watch();
  std::lock_guard<std::mutex> lock(w.mutex);
  for (bool h : w.held)
    if (h) return true;
  return false;
}

enum class LrKbState { kOff, kBrowse, kWaitRelease, kCapture, kSwapAsk, kRestoreAsk };

struct LrKeyboardView {
  LrKbState state = LrKbState::kOff;
  uint32_t menu = 0;
  uint32_t column = 0;  // 0 left, 1 right
  // The Settings list as it was, to come back to.
  uint32_t saved_ids[21], saved_values[21], saved_count = 0, saved_cursor = 0, saved_first = 0;
  uint32_t slot = 0;            // command being assigned
  std::string new_key;          // its new key, while the swap question is open
  uint32_t other_slot = 0;      // the command that has that key
  std::string message;          // help bar text while capturing / after an invalid key
  int invalid_frames = 0;       // "cannot be assigned" shown for this many more frames
  bool dialog_ours = false;     // the menu's Yes/No dialog is one of ours
  uint32_t downs = 0;           // key-down count when the capture started
  int wait_frames = 0;          // frames spent waiting for the keys to be released
};
LrKeyboardView g_kb;

std::string lr_binding(uint32_t slot) { return rex::cvar::GetFlagByName(kKeySlotCvars[slot]); }

// keybind cvar + keyboard.txt (the line "name = value" replaced or appended).
void lr_set_binding(uint32_t slot, const std::string& key) {
  const char* cvar = kKeySlotCvars[slot];
  rex::cvar::SetFlagByName(cvar, key);
  if (g_lr_settings_dir.empty()) return;
  std::filesystem::path path = g_lr_settings_dir / "keyboard.txt";
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }
  bool replaced = false;
  for (std::string& line : lines) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string name = line.substr(i, eq - i);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    if (name == cvar) {
      line = std::string(cvar) + " = " + key;
      replaced = true;
    }
  }
  if (!replaced) lines.push_back(std::string(cvar) + " = " + key);
  std::ofstream out(path, std::ios::trunc);
  for (const std::string& line : lines) out << line << "\n";
  REXLOG_INFO("LR keyboard: {} = {}", cvar, key);
}

// "Character movement (forward): " + icon; highlighted when selected.
std::string lr_command_cell(uint32_t slot, bool selected) {
  std::string s;
  if (selected) s += "\xF9" "A";
  s += kKeyCommands[slot].text;
  s += ": ";
  if (selected) s += "\xF9" "@";
  s += '\xF1';
  s += kKeyCommands[slot].icon;
  return s;
}

void lr_play_sound(PPCContext& ctx, uint8_t* base, uint32_t sound) {
  GuestFrame frame(ctx, base);
  frame.Open();
  ctx.r3.u64 = ld32(base, kGuiManager);
  ctx.r4.u64 = sound;
  sub_82715CD0(ctx, base);
}

REX_EXTERN(sub_82790BE8);  // ConfigList: fill all rows again (list)

void lr_refresh_rows(PPCContext& ctx, uint8_t* base, uint32_t list) {
  GuestFrame frame(ctx, base);
  frame.Open();
  ctx.r3.u64 = list;
  sub_82790BE8(ctx, base);
}

void lr_refresh_help(PPCContext& ctx, uint8_t* base, uint32_t menu) {
  GuestFrame frame(ctx, base);
  frame.Open();
  ctx.r3.u64 = menu;
  sub_8278DC38(ctx, base);
}

uint32_t lr_kb_cursor(uint8_t* base) {
  uint32_t list = ld32(base, g_kb.menu + kMenuList);
  uint32_t cursor = list ? ld32(base, list + kConfigListCursor) : 0;
  return cursor < kKeyboardRows ? cursor : 0;
}

uint32_t lr_kb_selected_slot(uint8_t* base) { return lr_kb_cursor(base) + (g_kb.column ? kKeyboardRows : 0); }

void lr_kb_enter(PPCContext& ctx, uint8_t* base, uint32_t menu) {
  uint32_t list = ld32(base, menu + kMenuList);
  uint32_t items = list + kConfigListItems;
  g_kb.saved_count = std::min<uint32_t>(ld32(base, items + kListCount), 21);
  for (uint32_t i = 0; i < g_kb.saved_count; ++i) {
    g_kb.saved_ids[i] = ld32(base, items + kListIds + 4 * i);
    g_kb.saved_values[i] = ld32(base, items + kListValues + 4 * i);
  }
  g_kb.saved_cursor = ld32(base, list + kConfigListCursor);
  g_kb.saved_first = ld32(base, list + 0x1B0);
  for (uint32_t r = 0; r < kKeyboardRows; ++r) {
    st32(base, items + kListIds + 4 * r, kIdKeyboardRow + r);
    st32(base, items + kListValues + 4 * r, 0);
  }
  st32(base, items + kListCount, kKeyboardRows);
  st32(base, list + kConfigListCursor, 0);
  st32(base, list + 0x1B0, 0);
  lr_key_watch();
  g_kb.menu = menu;
  g_kb.column = 0;
  g_kb.state = LrKbState::kBrowse;
  g_kb.message.clear();
  LrKeyIconsFrame(base);  // the key icons before the rows are laid out
  lr_refresh_rows(ctx, base, list);
  lr_refresh_help(ctx, base, menu);
  lr_play_sound(ctx, base, kSoundConfirm);
  REXLOG_INFO("LR keyboard: Keyboard Settings opened");
}

// The rows' text x positions (+0x60, +0x70) before the view moved them, by text node.
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_kb_saved_x;

void lr_kb_leave(PPCContext& ctx, uint8_t* base) {
  for (const auto& [node, x] : g_kb_saved_x) {
    st32(base, node + 0x60, x.first);
    st32(base, node + 0x70, x.second);
  }
  g_kb_saved_x.clear();
  uint32_t menu = g_kb.menu;
  uint32_t list = ld32(base, menu + kMenuList);
  uint32_t items = list + kConfigListItems;
  for (uint32_t i = 0; i < g_kb.saved_count; ++i) {
    st32(base, items + kListIds + 4 * i, g_kb.saved_ids[i]);
    st32(base, items + kListValues + 4 * i, g_kb.saved_values[i]);
  }
  st32(base, items + kListCount, g_kb.saved_count);
  st32(base, list + kConfigListCursor, g_kb.saved_cursor);
  st32(base, list + 0x1B0, g_kb.saved_first);
  g_kb.state = LrKbState::kOff;
  g_kb.menu = 0;
  g_lr_input_blocked.store(false);
  LrKeyIconsFrame(base);  // back to controller icons unless playing with the keyboard
  lr_refresh_rows(ctx, base, list);
  lr_refresh_help(ctx, base, menu);
  lr_play_sound(ctx, base, kSoundCancel);
}

// The help bar is one line: Steam's two-line prompt, joined.
std::string lr_kb_prompt() {
  return std::string("Please press the key you wish to assign to \"") + kKeyCommands[g_kb.slot].text +
         "\". The current key is \xF1" + kKeyCommands[g_kb.slot].icon + ".";
}

// The game sees no input until every key is up again: the key just pressed may now be
// bound to another button (after a swap, Return can be B and would leave the view).
void lr_kb_block_until_released() {
  g_lr_input_blocked.store(true);
  std::thread([] {
    for (int i = 0; i < 300 && lr_any_key_held(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_kb.state != LrKbState::kWaitRelease && g_kb.state != LrKbState::kCapture) g_lr_input_blocked.store(false);
  }).detach();
}

// The menu's own Yes/No dialog with our question.
void lr_kb_ask(PPCContext& ctx, uint8_t* base, const std::string& question) {
  std::string choices = lr_resolved_text(ctx, base, "$f_con_def_01");  // the game's "Yes|No"
  std::string all = question + "|" + choices;
  GuestFrame frame(ctx, base);
  uint32_t text = frame.PushString(all.c_str());
  frame.Open();
  ctx.r3.u64 = g_kb.menu;
  ctx.r4.u64 = text;
  sub_8278C430(ctx, base);
  ctx.r3.u64 = ld32(base, kGuiManager);
  ctx.r4.u64 = kSoundDialog;
  sub_82715CD0(ctx, base);
  g_kb.dialog_ours = ld32(base, g_kb.menu + kMenuDialog) != 0;
}

void lr_kb_assign(PPCContext& ctx, uint8_t* base, const std::string& key) {
  uint32_t list = ld32(base, g_kb.menu + kMenuList);
  // Another command with this key: ask to swap.
  for (uint32_t s = 0; s < 24; ++s) {
    if (s == g_kb.slot) continue;
    std::string bound = lr_binding(s);
    if (lr_dik_for(bound) && lr_dik_for(bound) == lr_dik_for(key)) {
      g_kb.new_key = key;
      g_kb.other_slot = s;
      g_kb.state = LrKbState::kSwapAsk;
      std::string q = "The \xF1";
      q += kKeyCommands[s].icon;
      q += " key is already being used for another function.@r@rThe command currently set for this key is:@r\"";
      q += kKeyCommands[s].text;
      q += "\"@rDo you wish to switch the settings to the current command?";
      lr_kb_ask(ctx, base, q);
      return;
    }
  }
  lr_set_binding(g_kb.slot, key);
  g_kb.state = LrKbState::kBrowse;
  lr_play_sound(ctx, base, kSoundConfirm);
  lr_refresh_rows(ctx, base, list);
  lr_refresh_help(ctx, base, g_kb.menu);
}

}  // namespace

// A key went down or up in the game window (the app's input listener, lightningreturns_app.h).
void LrKeyInput(rex::ui::VirtualKey key, bool down, bool repeat) {
  const unsigned vk = unsigned(key);
  if (vk == 0 || vk >= 256) return;
  LrKeyWatch& w = lr_key_watch();
  std::lock_guard<std::mutex> lock(w.mutex);
  w.held[vk] = down;
  if (down && !repeat) {
    ++w.downs;
    w.last = int(vk);
  }
}

// Keyboard Settings view: the item's A opens it (from the activation hook in lr_config_menu.inc).
bool LrKeyboardActivate(PPCContext& ctx, uint8_t* base, uint32_t menu, uint32_t id) {
  if (id == kIdKeyboard) {
    lr_kb_enter(ctx, base, menu);
    return true;
  }
  if (g_kb.state == LrKbState::kBrowse && g_kb.menu == menu) {
    g_kb.slot = lr_kb_selected_slot(base);
    g_kb.state = LrKbState::kWaitRelease;
    g_kb.wait_frames = 0;
    g_lr_input_blocked.store(true);
    std::string bound = lr_binding(g_kb.slot);
    g_kb.message = lr_kb_prompt();
    lr_play_sound(ctx, base, kSoundConfirm);
    lr_refresh_help(ctx, base, menu);
    return true;
  }
  return false;
}

// Row texts for the view's ids; false for other ids.
bool LrKeyboardRowTexts(uint8_t* base, uint32_t id, std::string& label, std::string& value) {
  if (g_kb.state == LrKbState::kOff || id < kIdKeyboardRow || id >= kIdKeyboardRow + kKeyboardRows) return false;
  uint32_t row = id - kIdKeyboardRow;
  bool here = lr_kb_cursor(base) == row;
  label = lr_command_cell(row, here && g_kb.column == 0);
  value = lr_command_cell(kKeyboardRows + row, here && g_kb.column == 1);
  return true;
}

// Help bar text in the view, or nullptr.
const char* LrKeyboardHelp() {
  if (g_kb.state == LrKbState::kOff) return nullptr;
  if (!g_kb.message.empty()) return g_kb.message.c_str();
  return "Assign keys for when using keyboard control.";
}

bool LrKeyboardViewOpen(uint32_t menu) { return g_kb.state != LrKbState::kOff && g_kb.menu == menu; }
bool LrKeyboardViewActive() { return g_kb.state != LrKbState::kOff; }

REX_EXTERN(__imp__sub_8278C358);  // B: close the menu
REX_HOOK_RAW(sub_8278C358) {
  if (LrKeyboardViewOpen(ctx.r3.u32)) {
    if (g_kb.state == LrKbState::kBrowse) lr_kb_leave(ctx, base);
    return;
  }
  __imp__sub_8278C358(ctx, base);
}

// Left / Right: page up / down in the list; in the view, the column.
void lr_kb_column(PPCContext& ctx, uint8_t* base, uint32_t column) {
  if (g_kb.state != LrKbState::kBrowse || g_kb.column == column) return;
  g_kb.column = column;
  lr_play_sound(ctx, base, kSoundCursor);
  lr_refresh_rows(ctx, base, ld32(base, g_kb.menu + kMenuList));
}
REX_EXTERN(__imp__sub_8278C228);
REX_HOOK_RAW(sub_8278C228) {
  if (LrKeyboardViewOpen(ctx.r3.u32)) {
    lr_kb_column(ctx, base, 0);
    return;
  }
  __imp__sub_8278C228(ctx, base);
}
REX_EXTERN(__imp__sub_8278C2C0);
REX_HOOK_RAW(sub_8278C2C0) {
  if (LrKeyboardViewOpen(ctx.r3.u32)) {
    lr_kb_column(ctx, base, 1);
    return;
  }
  __imp__sub_8278C2C0(ctx, base);
}

// Up / Down: the game's cursor; the rows are filled again so the highlight follows.
REX_EXTERN(__imp__sub_8278C0F8);
REX_HOOK_RAW(sub_8278C0F8) {
  uint32_t menu = ctx.r3.u32;
  if (LrKeyboardViewOpen(menu) && g_kb.state != LrKbState::kBrowse) return;
  __imp__sub_8278C0F8(ctx, base);
  if (LrKeyboardViewOpen(menu)) lr_refresh_rows(ctx, base, ld32(base, menu + kMenuList));
}
REX_EXTERN(__imp__sub_8278C190);
REX_HOOK_RAW(sub_8278C190) {
  uint32_t menu = ctx.r3.u32;
  if (LrKeyboardViewOpen(menu) && g_kb.state != LrKbState::kBrowse) return;
  __imp__sub_8278C190(ctx, base);
  if (LrKeyboardViewOpen(menu)) lr_refresh_rows(ctx, base, ld32(base, menu + kMenuList));
}

// X (Restore defaults): in the view, this key's default.
REX_EXTERN(__imp__sub_8278DB30);
REX_HOOK_RAW(sub_8278DB30) {
  if (LrKeyboardViewOpen(ctx.r3.u32)) {
    if (g_kb.state == LrKbState::kBrowse) {
      g_kb.slot = lr_kb_selected_slot(base);
      g_kb.state = LrKbState::kRestoreAsk;
      lr_kb_ask(ctx, base, "Do you want to restore the key to its default setting?");
    }
    return;
  }
  __imp__sub_8278DB30(ctx, base);
}

// Yes in our questions (the dialog's first choice calls sub_8278F4A8, "Restore defaults").
bool LrKeyboardDialogYes(PPCContext& ctx, uint8_t* base, uint32_t menu) {
  if (!LrKeyboardViewOpen(menu) || !g_kb.dialog_ours) return false;
  g_kb.dialog_ours = false;
  uint32_t list = ld32(base, menu + kMenuList);
  if (g_kb.state == LrKbState::kSwapAsk) {
    std::string old_key = lr_binding(g_kb.slot);
    lr_set_binding(g_kb.other_slot, old_key);
    lr_set_binding(g_kb.slot, g_kb.new_key);
  } else if (g_kb.state == LrKbState::kRestoreAsk) {
    std::string key = kKeyCommands[g_kb.slot].default_key;
    for (uint32_t s = 0; s < 24; ++s) {  // a default another command took: swap, as above
      if (s != g_kb.slot && lr_dik_for(lr_binding(s)) == lr_dik_for(key)) lr_set_binding(s, lr_binding(g_kb.slot));
    }
    lr_set_binding(g_kb.slot, key);
  }
  g_kb.state = LrKbState::kBrowse;
  g_kb.message.clear();
  lr_kb_block_until_released();
  lr_refresh_rows(ctx, base, list);
  lr_refresh_help(ctx, base, menu);
  return true;
}

// Once per menu step (sub_8278F7F0): key capture, dialogs closing with No.
void LrKeyboardStep(PPCContext& ctx, uint8_t* base, uint32_t menu) {
  static int controls_before = 0;  // the Gamepad Controls picture closed: the cancel sound
  const int controls = g_lr_controls_open.load();
  if (controls_before >= 2 && controls == 0) lr_play_sound(ctx, base, kSoundCancel);
  controls_before = controls;
  if (!LrKeyboardViewOpen(menu)) return;
  if ((g_kb.state == LrKbState::kSwapAsk || g_kb.state == LrKbState::kRestoreAsk) &&
      !ld32(base, menu + kMenuDialog)) {
    // Closed without Yes.
    g_kb.dialog_ours = false;
    g_kb.state = LrKbState::kBrowse;
    g_kb.message.clear();
    lr_refresh_help(ctx, base, menu);
    return;
  }
  if (g_kb.invalid_frames > 0 && --g_kb.invalid_frames == 0) {
    g_kb.message = lr_kb_prompt();
    lr_refresh_help(ctx, base, menu);
  }
  if (g_kb.state != LrKbState::kWaitRelease && g_kb.state != LrKbState::kCapture) return;
  LrKeyWatch& watch = lr_key_watch();
  if (g_kb.state == LrKbState::kWaitRelease) {
    // The key that chose the command is up (or half a second has passed: a key whose release
    // the window never reported must not block the capture).
    if (!lr_any_key_held() || ++g_kb.wait_frames > 30) {
      std::lock_guard<std::mutex> lock(watch.mutex);
      g_kb.downs = watch.downs;
      g_kb.wait_frames = 0;
      g_kb.state = LrKbState::kCapture;
      REXLOG_INFO("LR keyboard: waiting for a key for \"{}\"", kKeyCommands[g_kb.slot].text);
    }
    return;
  }
  int pressed = -1;
  {
    std::lock_guard<std::mutex> lock(watch.mutex);
    if (watch.downs != g_kb.downs) {
      g_kb.downs = watch.downs;
      pressed = watch.last;
    }
  }
  if (pressed >= 0) {
    const char* name = lr_key_name(rex::ui::VirtualKey(pressed));
    REXLOG_INFO("LR keyboard: key {:02X} pressed ({})", pressed, name ? name : "cannot be assigned");
    if (!name) {
      // Ctrl, Alt, Windows, Num Lock, Scroll Lock, ...: Steam's message for three seconds.
      g_kb.message = "This key cannot be assigned to game controls.";
      g_kb.invalid_frames = 90;
      g_kb.state = LrKbState::kWaitRelease;
      lr_play_sound(ctx, base, kSoundBuzzer);
      lr_refresh_help(ctx, base, menu);
      return;
    }
    g_kb.message.clear();
    g_kb.invalid_frames = 0;
    lr_kb_assign(ctx, base, name);
    lr_kb_block_until_released();
    return;
  }
}

// ---------------------------------------------------------------------------------------
// Gamepad Controls: the Steam version's controls picture (steam_ui/controls_<lang>.png),
// shown by the app over the game (LrShowControls in lightningreturns_app.h).
// ---------------------------------------------------------------------------------------
// 0 closed; 1 open, waiting for the button that opened it to be released; 2 open; 3 closing,
// waiting for the closing button's release (the input hook in lr_hooks.cpp moves 1 -> 2 -> 3 -> 0
// and hides the buttons from the game meanwhile).
std::atomic<int> g_lr_controls_open{0};
std::function<void(bool)> g_lr_controls_ui;
std::mutex g_lr_controls_mutex;
std::filesystem::path g_lr_controls_path;  // the picture: a 1024x480 PNG

std::filesystem::path LrControlsPicture() {
  std::lock_guard<std::mutex> lock(g_lr_controls_mutex);
  return g_lr_controls_path;
}

bool LrControlsOpen() {
  if (g_lr_settings_dir.empty()) return false;
  // The text language: XLanguage 4 French, 5 Spanish; English otherwise (the game's text sets).
  const int language = std::atoi(rex::cvar::GetFlagByName("user_language").c_str());
  const char* lang = language == 4 ? "fr" : language == 5 ? "sp" : "en";
  std::filesystem::path dir = g_lr_settings_dir / "steam_ui";
  std::filesystem::path path = dir / fmt::format("controls_{}.png", lang);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) path = dir / "controls_en.png";
  if (!std::filesystem::is_regular_file(path, ec)) {
    REXLOG_INFO("LR controls: no controls picture ({}); run the step script with the Steam version installed",
                path.string());
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(g_lr_controls_mutex);
    g_lr_controls_path = path;
  }
  g_lr_controls_open.store(1);
  if (g_lr_controls_ui) g_lr_controls_ui(true);
  REXLOG_INFO("LR controls: Gamepad Controls opened ({})", path.filename().string());
  return true;
}

// Steam's layout: each column's commands end at the same x (the icons in a column), left at
// 611 and right at 1158 of 1280. The label node is left-aligned at its x (+0x60, copy at +0x70);
// the value node centres its text in a box from x of twice +0xA0. +0x218 is a text's width.
void lr_kb_align(uint8_t* base, uint32_t row) {
  auto ldf = [&](uint32_t a) {
    uint32_t u = ld32(base, a);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  };
  auto stf = [&](uint32_t a, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    st32(base, a, u);
  };
  uint32_t label = ld32(base, row + 0xE4), value = ld32(base, row + 0xE8);
  for (uint32_t node : {label, value}) {
    if (node) g_kb_saved_x.try_emplace(node, ld32(base, node + 0x60), ld32(base, node + 0x70));
  }
  static int logged = 0;
  if (label) {
    float w = ldf(label + 0x218);
    if (w > 0.0f && w < 1000.0f) {
      stf(label + 0x60, 611.0f - w);
      stf(label + 0x70, 611.0f - w);
    }
    if (logged < 4) REXLOG_INFO("LR keyboard: row {:08X} left width {}", row, w);
  }
  if (value) {
    float w = ldf(value + 0x218), half = ldf(value + 0xA0);
    if (w > 0.0f && w < 1000.0f && half > 0.0f) {
      w = std::min(w, 2.0f * half);  // wider text is shrunk to the box
      float x = 1158.0f - w / 2.0f - half;
      stf(value + 0x60, x);
      stf(value + 0x70, x);
    }
    if (logged < 4) REXLOG_INFO("LR keyboard: row {:08X} right width {} half {}", row, w, half);
  }
  ++logged;
}
