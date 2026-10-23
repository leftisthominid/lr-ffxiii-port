// lightningreturns - imports the Steam (PC) version's saves.
//
// At start, every Steam save found (lrff13-NN.dat in Steam Cloud's folder, in
// %LOCALAPPDATA%\SquareEnix\LightningReturnsFinalFantasyXIII, or dropped into
// <settings>\steam_saves) is converted to the Xbox 360 format and added as a save slot. The game
// then loads it like any other save and saves it in the 360 format from then on.
//   - Steam files are only read, never changed.
//   - A save is imported once (steam_saves_imported.txt lists them by content), into the slot
//     with its Steam number when that is free, otherwise into the first free slot. Existing
//     saves are never overwritten.
//
// The two formats (both checked against real saves; 360 -> PC -> 360 reproduces a 360 save):
//   Steam lrff13-NN.dat (548000 bytes)      360 ff13-NN.dat\APP.DAT (547616 bytes)
//   header 0x210: 0x0D, u64 LE flags at 8   header 0x110: 0x0D, u64 BE flags at 8
//     (0x40_08002320 + progress << 4),        (0x40_08004320 + progress << 4),
//     playtime text at 0x10                   playtime text at 0x10
//   body: the FF XIII save cipher, PC key   body: the same cipher, Xbox key
//   28-entry section table, little-endian   the same table, big-endian
//   root entry id 0x2514DE2E                0x2514DE1E
//   gameconfig 0x90 bytes (0x80 PC-only)    gameconfig 0x10 bytes
//   section data big-endian on both, except the characters' equipment references (garb,
//   weapon, shield, accessories: indexes into DbEquipMan), which Steam stores with the two
//   16-bit halves swapped (garb 308: 01 34 00 00 on Steam, 00 00 01 34 on the 360; the 360 game
//   reads 0x01340000 and crashes loading the save). They are in db_party0000 (three sets of
//   eight from 0x80, 0x6E apart) and db_ptmem0000-0002 (eight at 0xBF7).
//   trailer [u32 LE length][u32 LE checksum]

#pragma once

#include <rex/logging.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lr_steam_saves {

constexpr size_t kPcSize = 548000, kPcHeader = 0x210;
constexpr size_t kXboxSize = 547616, kXboxHeader = 0x110;
constexpr uint64_t kPcFlagsBase = 0x0000004008002320ull, kXboxFlagsBase = 0x0000004008004320ull;
constexpr uint32_t kPcRootId = 0x2514DE2E, kXboxRootId = 0x2514DE1E;
constexpr uint64_t kPcKey = 0xE9334ADDE044073Dull;    // after the key setup
constexpr uint64_t kXboxSeed = 0x09F0D55DB8EA4BFDull;  // before it
constexpr int kSections = 28;
constexpr int kMaxSlot = 50;  // the Steam version's slots are 00-50

inline uint32_t ld_le(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
inline uint32_t ld_be(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline void st_le(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
inline void st_be(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}

// The FF XIII-family save cipher (as in bucanero's ff13-decrypter, GPL-3.0): 32 subkeys, each
// the previous times 5, applied per 8-byte block.
struct Subkey {
  uint32_t lo, hi;
  uint8_t delta;
};
inline std::vector<Subkey> Subkeys(uint64_t k) {
  std::vector<Subkey> keys;
  for (int i = 0; i < 32; ++i) {
    uint32_t sum = 0;
    for (int b = 0; b < 8; ++b) sum += uint8_t(k >> (8 * b));
    keys.push_back({uint32_t(k), uint32_t(k >> 32), uint8_t(0x78 * 8 - sum)});
    k *= 5;
  }
  return keys;
}
// The key setup the game applies to a console seed.
inline uint64_t SetupKey(uint64_t k) {
  k = ((k & 0xFF00000000000000ull) >> 16) | ((k & 0x0000FF0000000000ull) << 16) | (k & 0x00FF00FFFFFFFFFFull);
  k = ((k & 0x00000000FF00FF00ull) >> 8) | ((k & 0x0000000000FF00FFull) << 8) | (k & 0xFFFFFFFF00000000ull);
  uint8_t b[8];
  for (int i = 0; i < 8; ++i) b[i] = uint8_t(k >> (8 * i));
  b[0] = uint8_t(b[0] + 0x45);
  for (int i = 1; i < 8; ++i) b[i] = uint8_t(((b[i - 1] + b[i] + 0xD4) ^ ((b[i - 1] << 2) & 0x3FC)) ^ 0x45);
  k = 0;
  for (int i = 0; i < 8; ++i) k |= uint64_t(b[i]) << (8 * i);
  return k;
}
inline void Crypt(uint8_t* data, size_t size, uint64_t key, bool decrypt) {
  const std::vector<Subkey> keys = Subkeys(key);
  for (size_t j = 0; j < size / 8; ++j) {
    uint8_t* d = data + j * 8;
    const Subkey& k = keys[j % 32];
    const uint64_t o = j * 8;
    const uint64_t cog = o << 20;
    const uint32_t base = uint32_t(cog) | uint32_t(o << 10) | uint32_t(o);
    const uint32_t gear1 = base + 0xA1652347u;
    const uint32_t gear2 = (uint32_t(j * 2) | uint32_t(cog >> 32)) + (base > ~0xA1652347u ? 1u : 0u);
    if (decrypt) {
      uint8_t t[8], prev = 0;
      for (int i = 0; i < 8; ++i) {
        const uint8_t c = d[i];
        t[i] = uint8_t((i == 0 ? c ^ uint8_t(0x45 ^ j) : c ^ prev) + k.delta);
        prev = c;
      }
      const uint32_t a = ld_le(t), b = ld_le(t + 4);
      const uint32_t p2 = k.lo ^ gear1 ^ (a - k.lo);
      const uint32_t p1 = k.hi ^ gear2 ^ (b - k.hi - (a < k.lo ? 1u : 0u));
      st_le(d, p1);
      st_le(d + 4, p2);
    } else {
      const uint32_t p1 = ld_le(d), p2 = ld_le(d + 4);
      const uint32_t a = k.lo ^ gear1 ^ p2;
      const uint32_t b = k.hi ^ gear2 ^ p1;
      uint8_t t[8];
      st_le(t, a + k.lo);
      st_le(t + 4, b + k.hi + (a > ~k.lo ? 1u : 0u));
      uint8_t prev = 0;
      for (int i = 0; i < 8; ++i) {
        uint8_t c = uint8_t(t[i] - k.delta);
        c ^= i == 0 ? uint8_t(0x45 ^ j) : prev;
        d[i] = c;
        prev = c;
      }
    }
  }
}
// Byte 0 of every 32-bit word, all but the last 8 bytes.
inline uint32_t Checksum(const uint8_t* body, size_t size) {
  uint32_t sum = 0;
  for (size_t i = 0; i + 8 < size; i += 4) sum += body[i];
  return sum;
}

// Steam's equipment references -> the 360's (see the top). A reference already in the 360 form
// (saves assembled from 360 data) stays; anything that is no equipment index becomes "none".
// Returns how many were not valid.
inline int FixEquipmentRefs(uint8_t* section, size_t size, const uint8_t* name) {
  std::vector<size_t> refs;
  if (std::memcmp(name, "db_party0000\0", 13) == 0) {
    for (size_t set = 0; set < 3; ++set)
      for (size_t i = 0; i < 8; ++i) refs.push_back(0x80 + set * 0x6E + i * 4);
  } else if (std::memcmp(name, "db_ptmem000", 11) == 0 && name[12] == 0) {
    for (size_t i = 0; i < 8; ++i) refs.push_back(0xBF7 + i * 4);
  }
  constexpr uint32_t kEquipment = 632;  // the game's equipment table (616 + 16)
  int invalid = 0;
  for (size_t o : refs) {
    if (o + 4 > size) break;
    uint32_t v = ld_be(section + o);
    if (v == 0xFFFFFFFFu) continue;
    if ((v & 0xFFFF) == 0 && (v >> 16) < kEquipment) {
      v >>= 16;  // Steam form
    } else if (v >= kEquipment) {
      v = 0xFFFFFFFFu;
      ++invalid;
    }
    st_be(section + o, v);
  }
  return invalid;
}

// Steam lrff13-NN.dat -> 360 APP.DAT. False (and why) when the file is not a Steam save.
// fix_refs false: the conversion of steps 47-49 (equipment references copied as they were), to
// recognise slots imported that way.
inline bool SteamToXbox(const std::vector<uint8_t>& pc, std::vector<uint8_t>& out, std::string& why,
                        bool fix_refs = true) {
  if (pc.size() != kPcSize || pc[0] != 0x0D || std::memcmp(pc.data() + 0x10, "Total hours played:", 19) != 0) {
    why = "not a Lightning Returns Steam save";
    return false;
  }
  std::vector<uint8_t> body(pc.begin() + kPcHeader, pc.end());
  Crypt(body.data(), body.size(), kPcKey, true);
  if (std::memcmp(body.data(), "db_partyman\0", 12) != 0 ||
      ld_le(body.data() + body.size() - 4) != Checksum(body.data(), body.size())) {
    why = "does not decrypt (damaged, or another game version)";
    return false;
  }
  // Section table: 28 x {name[16], id, count, reserved, size}; data from 0x380 in table order.
  std::vector<uint8_t> xb;
  xb.reserve(kXboxSize - kXboxHeader);
  xb.resize(kSections * 32);
  size_t from = kSections * 32;
  bool root = false, config = false;
  std::vector<std::pair<size_t, size_t>> data;  // offset, size to copy
  for (int s = 0; s < kSections; ++s) {
    const uint8_t* e = body.data() + s * 32;
    uint8_t* x = xb.data() + s * 32;
    std::memcpy(x, e, 16);
    uint32_t f[4];
    for (int i = 0; i < 4; ++i) f[i] = ld_le(e + 16 + 4 * i);
    if (from + f[3] > body.size() - 8) {
      why = "section table out of range";
      return false;
    }
    size_t copy = f[3];
    if (std::memcmp(e, "db_partyman\0", 12) == 0) {
      if (f[0] != kPcRootId) {
        why = "unexpected save layout (root id)";
        return false;
      }
      f[0] = kXboxRootId;
      root = true;
    } else if (std::memcmp(e, "gameconfig\0", 11) == 0) {
      if (f[3] != 0x90) {
        why = "unexpected save layout (gameconfig)";
        return false;
      }
      f[3] = 0x10;  // the 360 part; the rest holds PC-only settings
      copy = 0x10;
      config = true;
    }
    for (int i = 0; i < 4; ++i) st_be(x + 16 + 4 * i, f[i]);
    data.emplace_back(from, copy);
    from += e[16 + 12] | e[16 + 13] << 8 | e[16 + 14] << 16 | uint32_t(e[16 + 15]) << 24;
  }
  if (!root || !config) {
    why = "unexpected save layout";
    return false;
  }
  for (size_t s = 0; s < data.size(); ++s) {
    const auto& [offset, size] = data[s];
    const size_t at = xb.size();
    xb.insert(xb.end(), body.begin() + offset, body.begin() + offset + size);
    if (fix_refs && FixEquipmentRefs(xb.data() + at, size, xb.data() + s * 32))
      REXLOG_WARN("LR Steam saves: invalid equipment references in {} cleared",
                  reinterpret_cast<const char*>(xb.data() + s * 32));
  }
  xb.insert(xb.end(), body.begin() + from, body.end() - 8);  // padding after the data
  xb.resize(xb.size() + 8);
  if (xb.size() != kXboxSize - kXboxHeader) {
    why = "unexpected save size";
    return false;
  }
  st_le(xb.data() + xb.size() - 8, uint32_t(xb.size() - 8));
  st_le(xb.data() + xb.size() - 4, Checksum(xb.data(), xb.size()));
  Crypt(xb.data(), xb.size(), SetupKey(kXboxSeed), false);

  uint64_t flags = 0;
  for (int i = 0; i < 8; ++i) flags |= uint64_t(pc[8 + i]) << (8 * i);
  const uint64_t progress = flags - kPcFlagsBase < 0x1000 ? flags - kPcFlagsBase : 0x10;
  out.assign(kXboxHeader, 0);
  out[0] = 0x0D;
  const uint64_t xflags = kXboxFlagsBase + progress;
  for (int i = 0; i < 8; ++i) out[8 + i] = uint8_t(xflags >> (56 - 8 * i));
  std::memcpy(out.data() + 0x10, pc.data() + 0x10, 0x100);
  out.insert(out.end(), xb.begin(), xb.end());

  // Check the result the way the game reads it.
  std::vector<uint8_t> check(out.begin() + kXboxHeader, out.end());
  Crypt(check.data(), check.size(), SetupKey(kXboxSeed), true);
  if (std::memcmp(check.data(), "db_partyman\0", 12) != 0 ||
      ld_le(check.data() + check.size() - 4) != Checksum(check.data(), check.size())) {
    why = "conversion self-check failed";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------
// DLC items the 360 game does not have. A Steam save can hold DLC garbs, weapons, shields, and
// accessories whose 360 pack is not installed; the game lists those ids itself ("items held back",
// written to dlc_unavailable.txt by the DLC hook). They are taken out of an imported save:
//   DbEquipMan: 1026 rows of 18 bytes {name[14] ("cos_oa00"), u32}, then 12 bytes;
//   ItemBagWp / Shi / Acc / Cos: u32 capacity, then 18-byte records {key[14], u32 count}; the key
//     encodes a DbEquipMan row: 7 bytes 0x30/0xB0 (bits of the ASCII hex digit of row % 16),
//     '_', 6 hex digits of row / 16;
//   equipped items: DbEquipMan rows in db_party0000 / db_ptmem000N (see FixEquipmentRefs).
// An equipped item that goes is replaced by an owned item of the same kind (cos_ / wea_ / shi_ /
// acc_) that is not equipped elsewhere; with none, the slot is emptied.
// ---------------------------------------------------------------------------------------
constexpr size_t kEquipRows = 1026, kEquipRow = 18, kBagRecord = 18;

inline int DecodeGearKey(const uint8_t* k) {
  if (k[7] != '_') return -1;
  int v = 0;
  for (int i = 0; i < 7; ++i) {
    if (k[i] != 0x30 && k[i] != 0xB0) return -1;
    v = 2 * v + (k[i] == 0xB0);
  }
  const char* hex = "0123456789abcdef";
  const char* d = std::strchr(hex, v);
  if (!v || !d) return -1;
  int hi = 0;
  for (int i = 8; i < 14; ++i) {
    const char* h = std::strchr(hex, k[i]);
    if (!k[i] || !h) return -1;
    hi = hi * 16 + int(h - hex);
  }
  return hi * 16 + int(d - hex);
}

inline std::set<std::string> ReadUnavailable(const std::filesystem::path& settings_dir) {
  std::set<std::string> names;
  std::ifstream in(settings_dir / "dlc_unavailable.txt");
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.size() > 3 && line[0] != '#') names.insert(line);
  }
  return names;
}

// Takes the unavailable items out of a 360 APP.DAT. Returns how many items were removed (0: the
// file is unchanged); `report` names them.
inline int RemoveUnavailable(std::vector<uint8_t>& app, const std::set<std::string>& unavailable, std::string& report) {
  if (unavailable.empty() || app.size() != kXboxSize) return 0;
  std::vector<uint8_t> body(app.begin() + kXboxHeader, app.end());
  const uint64_t key = SetupKey(kXboxSeed);
  Crypt(body.data(), body.size(), key, true);
  if (std::memcmp(body.data(), "db_partyman\0", 12) != 0 ||
      ld_le(body.data() + body.size() - 4) != Checksum(body.data(), body.size()))
    return 0;
  std::map<std::string, std::pair<size_t, size_t>> sec;  // name -> offset, size
  size_t from = kSections * 32;
  for (int s = 0; s < kSections; ++s) {
    const uint8_t* e = body.data() + s * 32;
    const size_t size = ld_be(e + 28);
    if (from + size > body.size() - 8) return 0;
    sec[std::string(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 16))] = {from, size};
    from += size;
  }
  auto it = sec.find("DbEquipMan");
  if (it == sec.end() || it->second.second < kEquipRows * kEquipRow) return 0;
  uint8_t* rows = body.data() + it->second.first;
  auto row_name = [&](size_t i) {
    return std::string(reinterpret_cast<const char*>(rows + i * kEquipRow), strnlen(reinterpret_cast<const char*>(rows + i * kEquipRow), 14));
  };
  std::set<int> bad;
  for (size_t i = 0; i < kEquipRows; ++i)
    if (unavailable.count(row_name(i))) bad.insert(int(i));
  if (bad.empty()) return 0;
  // Bags: owned items (for replacements), and the records of removed ones.
  std::map<std::string, std::vector<int>> owned;  // kind -> rows
  for (const char* bag : {"ItemBagCos", "ItemBagWp", "ItemBagShi", "ItemBagAcc"}) {
    auto b = sec.find(bag);
    if (b == sec.end() || b->second.second < 4) continue;
    uint8_t* recs = body.data() + b->second.first + 4;
    const size_t n = (b->second.second - 4) / kBagRecord;
    for (size_t r = 0; r < n; ++r) {
      uint8_t* rec = recs + r * kBagRecord;
      const int row = DecodeGearKey(rec);
      if (row < 0 || row >= int(kEquipRows) || !ld_be(rec + 14)) continue;
      if (bad.count(row)) {
        std::memset(rec, 0, kBagRecord);
      } else {
        owned[row_name(size_t(row)).substr(0, 4)].push_back(row);
      }
    }
  }
  // Equipped references.
  std::vector<uint8_t*> refs;
  for (auto& [name, where] : sec) {
    if (name == "db_party0000") {
      for (size_t set = 0; set < 3; ++set)
        for (size_t i = 0; i < 8; ++i) refs.push_back(body.data() + where.first + 0x80 + set * 0x6E + i * 4);
    } else if (name.rfind("db_ptmem000", 0) == 0 && name.size() == 12) {
      for (size_t i = 0; i < 8; ++i) refs.push_back(body.data() + where.first + 0xBF7 + i * 4);
    }
  }
  std::set<int> equipped;
  std::set<std::string> equipped_names;
  for (uint8_t* r : refs) {
    const uint32_t v = ld_be(r);
    equipped.insert(int(v));
    if (v < kEquipRows && !bad.count(int(v))) equipped_names.insert(row_name(v));
  }
  // One replacement per removed item (the party and character sections name the same items),
  // preferring an owned item of that kind whose name is not worn elsewhere.
  std::map<uint32_t, uint32_t> replacement;
  int replaced = 0;
  for (uint8_t* r : refs) {
    const uint32_t v = ld_be(r);
    if (!bad.count(int(v))) continue;
    auto known = replacement.find(v);
    if (known == replacement.end()) {
      const std::string kind = row_name(v).substr(0, 4);
      uint32_t pick = 0xFFFFFFFFu;
      for (int pass = 0; pass < 2 && pick == 0xFFFFFFFFu; ++pass)
        for (int c : owned[kind])
          if (!equipped.count(c) && (pass || !equipped_names.count(row_name(size_t(c))))) { pick = uint32_t(c); break; }
      if (pick == 0xFFFFFFFFu && !owned[kind].empty()) pick = uint32_t(owned[kind][0]);
      if (pick != 0xFFFFFFFFu) {
        equipped.insert(int(pick));
        equipped_names.insert(row_name(pick));
      }
      known = replacement.emplace(v, pick).first;
      ++replaced;
    }
    st_be(r, known->second);
  }
  std::map<std::string, int> gone;
  for (int i : bad) {
    ++gone[row_name(size_t(i))];
    std::memset(rows + size_t(i) * kEquipRow, 0, kEquipRow);
  }
  for (const auto& [name, count] : gone)
    report += (report.empty() ? "" : ", ") + name + (count > 1 ? " x" + std::to_string(count) : "");
  if (replaced) report += " (" + std::to_string(replaced) + " were equipped; replaced by owned items)";
  const int removed = int(bad.size());
  st_le(body.data() + body.size() - 4, Checksum(body.data(), body.size()));
  Crypt(body.data(), body.size(), key, false);
  std::copy(body.begin(), body.end(), app.begin() + kXboxHeader);
  return removed;
}

// The imported Steam and PS3 slots (steam_saves_imported.txt, ps3_saves_imported.txt: "<hash> <slot>
// ..."), cleaned of unavailable DLC items at every start: a save imported while a pack was
// installed loses that pack's items once it is gone. The file as it was is kept once as
// APP.DAT.before_dlc_cleanup.
inline void CleanImported(const std::filesystem::path& settings_dir, const std::set<std::string>& unavailable) {
  const std::filesystem::path saves = settings_dir / "B13EBABEBABEBABE" / "5351081B" / "00000001";
  std::map<std::string, const char*> slots;  // slot -> where it came from
  for (const auto& [file, from] : {std::pair{"steam_saves_imported.txt", "Steam"}, std::pair{"ps3_saves_imported.txt", "PS3"}}) {
    std::ifstream list(settings_dir / file);
    std::string line;
    while (std::getline(list, line))
      if (line.size() > 17) slots[line.substr(17, line.find(' ', 17) - 17)] = from;
  }
  std::error_code ec;
  for (const auto& [slot, from] : slots) {
    const std::filesystem::path app_path = saves / slot / "APP.DAT";
    std::ifstream in(app_path, std::ios::binary);
    std::vector<uint8_t> app((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::string report;
    if (!RemoveUnavailable(app, unavailable, report)) continue;
    const std::filesystem::path backup = saves / slot / "APP.DAT.before_dlc_cleanup";
    if (!std::filesystem::exists(backup, ec)) std::filesystem::copy_file(app_path, backup, ec);
    std::ofstream out(app_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(app.data()), std::streamsize(app.size()));
    if (out.good()) REXLOG_INFO("LR {} saves: {}: DLC items the 360 game does not have removed: {}", from, slot, report);
  }
}

inline uint64_t Fnv64(const std::vector<uint8_t>& d) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint8_t c : d) h = (h ^ c) * 0x100000001B3ull;
  return h;
}

// The 360 content header our runtime keeps next to a save (as LR_Import_360_Saves.ps1 writes).
inline std::vector<uint8_t> ContentHeader(const std::string& file_name, int slot) {
  std::vector<uint8_t> h(0x148, 0);
  h[3] = 1;  // device: HDD
  h[7] = 1;  // content type: saved game
  const std::string display = "Save Data " + std::string(slot + 1 < 10 ? "0" : "") + std::to_string(slot + 1);
  for (size_t i = 0; i < display.size() && i < 127; ++i) h[8 + 2 * i + 1] = uint8_t(display[i]);  // UTF-16BE
  std::memcpy(h.data() + 0x108, file_name.data(), std::min<size_t>(file_name.size(), 41));
  const uint64_t xuid = 0xB13EBABEBABEBABEull;
  for (int i = 0; i < 8; ++i) h[0x138 + i] = uint8_t(xuid >> (56 - 8 * i));
  st_be(h.data() + 0x140, 0x5351081B);
  return h;
}

inline std::vector<std::filesystem::path> SteamSaveFiles(const std::filesystem::path& settings_dir) {
  std::vector<std::filesystem::path> dirs;
  std::error_code ec;
#if defined(_WIN32)
  // Steam Cloud's local copy: <Steam>\userdata\<account>\345350\remote, Steam in its default
  // folder (only this game's folder is read; no registry or other Steam files).
  const std::filesystem::path steam = "C:/Program Files (x86)/Steam";
  for (const auto& account : std::filesystem::directory_iterator(steam / "userdata", ec)) {
    dirs.push_back(account.path() / "345350" / "remote");
  }
  // Without Steam Cloud: the game's own folder.
  for (const char* var : {"LOCALAPPDATA", "APPDATA"}) {
    if (const char* v = std::getenv(var); v && v[0]) {
      dirs.push_back(std::filesystem::path(v) / "SquareEnix" / "LightningReturnsFinalFantasyXIII");
    }
  }
#endif
  dirs.push_back(settings_dir / "steam_saves");  // copied in by hand
  std::vector<std::filesystem::path> files;
  for (const auto& dir : dirs) {
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      const std::string name = entry.path().filename().string();
      int slot = -1;
      if (entry.is_regular_file(ec) && std::sscanf(name.c_str(), "lrff13-%d.dat", &slot) == 1 && slot >= 0 &&
          slot <= kMaxSlot && name.size() == 13) {
        files.push_back(entry.path());
      }
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

// settings_dir: the user data folder (saves under B13EBABEBABEBABE\5351081B).
inline void Import(const std::filesystem::path& settings_dir) {
  const std::filesystem::path saves = settings_dir / "B13EBABEBABEBABE" / "5351081B" / "00000001";
  const std::filesystem::path headers = settings_dir / "B13EBABEBABEBABE" / "5351081B" / "Headers" / "00000001";
  const std::filesystem::path list_path = settings_dir / "steam_saves_imported.txt";
  std::set<std::string> imported;
  std::map<std::string, std::string> imported_slot;  // hash -> ff13-NN.dat
  {
    std::ifstream list(list_path);
    std::string line;
    while (std::getline(list, line)) {
      if (line.size() < 16) continue;
      imported.insert(line.substr(0, 16));
      if (line.size() > 17) imported_slot[line.substr(0, 16)] = line.substr(17, line.find(' ', 17) - 17);
    }
  }
  std::error_code ec;
  auto slot_used = [&](int slot) {
    char name[32];
    std::snprintf(name, sizeof(name), "ff13-%02d.dat", slot);
    return std::filesystem::exists(saves / name, ec) || std::filesystem::exists(headers / (std::string(name) + ".header"), ec);
  };
  int added = 0, found = 0, repaired = 0;
  for (const auto& path : SteamSaveFiles(settings_dir)) {
    ++found;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> pc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llX", static_cast<unsigned long long>(Fnv64(pc)));
    if (imported.count(hash)) {
      // Imported by steps 47-49 with the Steam equipment references: rewrite the slot if the
      // game has not saved into it since (it is still exactly that conversion).
      auto it = imported_slot.find(hash);
      std::vector<uint8_t> before, after;
      std::string why;
      if (it == imported_slot.end() || !SteamToXbox(pc, before, why, false) || !SteamToXbox(pc, after, why)) continue;
      if (before == after) continue;
      const std::filesystem::path app_path = saves / it->second / "APP.DAT";
      std::ifstream old_in(app_path, std::ios::binary);
      std::vector<uint8_t> current((std::istreambuf_iterator<char>(old_in)), std::istreambuf_iterator<char>());
      old_in.close();
      if (current != before) continue;
      std::ofstream app(app_path, std::ios::binary | std::ios::trunc);
      app.write(reinterpret_cast<const char*>(after.data()), std::streamsize(after.size()));
      if (app.good()) {
        ++repaired;
        REXLOG_INFO("LR Steam saves: {} repaired (equipment references converted)", it->second);
      }
      continue;
    }
    std::vector<uint8_t> xbox;
    std::string why;
    if (!SteamToXbox(pc, xbox, why)) {
      REXLOG_WARN("LR Steam saves: {} not imported: {}", path.string(), why);
      continue;
    }
    // DLC items the 360 game does not have (as the game listed them on its last run).
    std::string removed;
    if (RemoveUnavailable(xbox, ReadUnavailable(settings_dir), removed))
      REXLOG_INFO("LR Steam saves: {}: DLC items the 360 game does not have removed: {}", path.string(), removed);
    int slot = -1;
    std::sscanf(path.filename().string().c_str(), "lrff13-%d.dat", &slot);
    if (slot_used(slot)) {
      slot = -1;
      for (int s = 0; s <= kMaxSlot && slot < 0; ++s) {
        if (!slot_used(s)) slot = s;
      }
    }
    if (slot < 0) {
      REXLOG_WARN("LR Steam saves: {} not imported: no free save slot", path.string());
      continue;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "ff13-%02d.dat", slot);
    std::filesystem::create_directories(saves / name, ec);
    std::filesystem::create_directories(headers, ec);
    const std::vector<uint8_t> header = ContentHeader(name, slot);
    std::ofstream app(saves / name / "APP.DAT", std::ios::binary);
    app.write(reinterpret_cast<const char*>(xbox.data()), std::streamsize(xbox.size()));
    std::ofstream head(headers / (std::string(name) + ".header"), std::ios::binary);
    head.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    if (!app.good() || !head.good()) {
      REXLOG_WARN("LR Steam saves: could not write slot {}", name);
      continue;
    }
    std::ofstream list(list_path, std::ios::app);
    list << hash << ' ' << name << ' ' << path.string() << '\n';
    imported.insert(hash);
    ++added;
    REXLOG_INFO("LR Steam saves: {} -> {} (Save Data {:02d}, {})", path.string(), name, slot + 1,
                std::string(reinterpret_cast<const char*>(pc.data()) + 0x10,
                            strnlen(reinterpret_cast<const char*>(pc.data()) + 0x10, 0x100)));
  }
  if (found) REXLOG_INFO("LR Steam saves: {} found, {} newly imported, {} repaired", found, added, repaired);
}

}  // namespace lr_steam_saves
