// lightningreturns - imports PS3 saves (experimental).
//
// At start, every PS3 save folder dropped into <settings>\ps3_saves (BLUS31311-01, BLJM60558-29,
// ... as copied off a PS3 to USB, or from RPCS3's dev_hdd0\home\<user>\savedata) is converted to
// the Xbox 360 format and added as a save slot, the way Steam saves are (lr_steam_saves.h):
//   - PS3 files are only read, never changed.
//   - A save is imported once (ps3_saves_imported.txt lists them by content), into the slot with
//     its PS3 number when that is free, otherwise into the first free slot. Existing saves are
//     never overwritten.
//
// The PS3 format (checked against a real PS3 save and three RPCS3 saves of BLJM60558):
//   PARAM.PFD present (a real PS3): KEY.DAT and APP.DAT are encrypted with the PS3 system's
//     save protection on top (AES-128, the per-file key held in PARAM.PFD under the game's
//     secure file id). RPCS3 writes them without it.
//   KEY.DAT: 16 bytes; the save key is (bytes 0-7 XOR bytes 8-15, big-endian) | 1, XOR the
//     game's constant, then the key setup of the FF XIII-family save cipher.
//   APP.DAT (547904 bytes): no plain header; the whole file is the cipher body.
//   The body is the 360 body (big-endian, the same 28 sections), except:
//     root entry id 0x2514DE5E (360 0x2514DE1E);
//     db_party0000 0x770 bytes (360 0x768), db_bt_result 0xD88 (0xB68), fieldobj 0x390 (0x388):
//     the 360 data, then bytes the PS3 version keeps zero (checked; a save with anything there
//     is not imported).
//   The 360 file's plain header (playtime text, progress) is rebuilt: the playtime text from
//   PARAM.SFO (DETAIL), the progress from the in-game clock (u64 at body 0x136A, 12500 per
//   second): day << 4, as a 360 save at day 1 holds 0x10.

#pragma once

#include "lr_steam_saves.h"

namespace lr_ps3_saves {

using lr_steam_saves::ld_be;
using lr_steam_saves::ld_le;
using lr_steam_saves::st_be;
using lr_steam_saves::st_le;

constexpr size_t kPs3Size = 547904;
constexpr uint32_t kPs3RootId = 0x2514DE5E;
// The save cipher's constant for this game and the KEY.DAT key derivation: bucanero's apollo-lib
// (source/decrypt.c, ff13_init_key, FFXIII_3_KEY in include/keys.h; GPL-3.0),
// https://github.com/bucanero/apollo-lib
constexpr uint64_t kGameKey = 0x36545E6CEB9A705Full;
constexpr uint64_t kClockPerSecond = 12500;
// The PS3 system's save protection: the syscon manager key and this game's secure file id
// (the same for every region of the game). Both are public:
//   syscon manager key - flatz's pfdtool (global.conf) and bucanero's Apollo Save Tool
//     (apollo-ps3, source/pfd_util.c, stored there XORed with D4D16B0C5DB08791);
//     https://github.com/bucanero/apollo-ps3, https://github.com/bucanero/pfd_sfo_tools
//   secure file id - Apollo's PS3 games.conf, Lightning Returns entry ("thanks to cp25stack");
//     https://github.com/bucanero/apollo-patches (PS3/games.conf)
// The PFD layout and the file decryption follow pfdtool (pfd.c).
constexpr uint8_t kSysconManagerKey[16] = {0xD4, 0x13, 0xB8, 0x96, 0x63, 0xE1, 0xFE, 0x9F,
                                           0x75, 0x14, 0x3D, 0x3B, 0xB4, 0x56, 0x52, 0x74};
constexpr uint8_t kSecureFileId[16] = {0x92, 0x1D, 0x3E, 0x04, 0x25, 0x25, 0xCB, 0x84,
                                       0x93, 0xB0, 0x2A, 0xC7, 0x39, 0x67, 0xF6, 0xDF};
struct Resize {
  const char* name;
  uint32_t ps3, xbox;
};
constexpr Resize kResized[] = {{"db_party0000", 0x770, 0x768}, {"db_bt_result", 0xD88, 0xB68}, {"fieldobj", 0x390, 0x388}};

// AES-128 (one block at a time), for the PS3 save protection.
class Aes128 {
 public:
  explicit Aes128(const uint8_t key[16]) {
    Tables();
    std::memcpy(rk_, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t t[4] = {rk_[i - 4], rk_[i - 3], rk_[i - 2], rk_[i - 1]};
      if (i % 16 == 0) {
        const uint8_t f = t[0];
        t[0] = uint8_t(sbox_[t[1]] ^ rcon);
        t[1] = sbox_[t[2]];
        t[2] = sbox_[t[3]];
        t[3] = sbox_[f];
        rcon = Xtime(rcon);
      }
      for (int j = 0; j < 4; ++j) rk_[i + j] = uint8_t(rk_[i - 16 + j] ^ t[j]);
    }
  }
  void Encrypt(uint8_t b[16]) const {
    AddKey(b, 0);
    for (int r = 1; r <= 10; ++r) {
      for (int i = 0; i < 16; ++i) b[i] = sbox_[b[i]];
      Shift(b, false);
      if (r != 10) MixColumns(b, false);
      AddKey(b, r);
    }
  }
  void Decrypt(uint8_t b[16]) const {
    AddKey(b, 10);
    for (int r = 9; r >= 0; --r) {
      Shift(b, true);
      for (int i = 0; i < 16; ++i) b[i] = inv_sbox_[b[i]];
      AddKey(b, r);
      if (r) MixColumns(b, true);
    }
  }

 private:
  static uint8_t Xtime(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
  static uint8_t Mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (; b; b >>= 1, a = Xtime(a))
      if (b & 1) p ^= a;
    return p;
  }
  static void Tables() {
    if (sbox_[0]) return;
    for (int x = 0; x < 256; ++x) {
      uint8_t inv = 0;
      for (int y = 1; y < 256 && x; ++y)
        if (Mul(uint8_t(x), uint8_t(y)) == 1) inv = uint8_t(y);
      uint8_t s = inv;
      for (int i = 1; i < 5; ++i) s ^= uint8_t((inv << i) | (inv >> (8 - i)));
      s ^= 0x63;
      sbox_[x] = s;
      inv_sbox_[s] = uint8_t(x);
    }
  }
  void AddKey(uint8_t b[16], int r) const {
    for (int i = 0; i < 16; ++i) b[i] ^= rk_[r * 16 + i];
  }
  static void Shift(uint8_t b[16], bool inverse) {
    uint8_t t[16];
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) {
        const int from = inverse ? ((c - r + 4) % 4) : ((c + r) % 4);
        t[c * 4 + r] = b[from * 4 + r];
      }
    std::memcpy(b, t, 16);
  }
  static void MixColumns(uint8_t b[16], bool inverse) {
    const uint8_t m[4] = {uint8_t(inverse ? 14 : 2), uint8_t(inverse ? 11 : 3), uint8_t(inverse ? 13 : 1),
                          uint8_t(inverse ? 9 : 1)};
    for (int c = 0; c < 4; ++c) {
      uint8_t* col = b + c * 4;
      uint8_t t[4];
      for (int r = 0; r < 4; ++r)
        t[r] = uint8_t(Mul(col[r], m[0]) ^ Mul(col[(r + 1) % 4], m[1]) ^ Mul(col[(r + 2) % 4], m[2]) ^
                       Mul(col[(r + 3) % 4], m[3]));
      std::memcpy(col, t, 4);
    }
  }
  uint8_t rk_[176];
  static inline uint8_t sbox_[256] = {};
  static inline uint8_t inv_sbox_[256] = {};
};

// A file protected by PARAM.PFD -> its plain content (as pfdtool decrypts it). False when the
// file is not listed there.
inline bool PfdDecrypt(const std::vector<uint8_t>& pfd, const std::string& file_name, std::vector<uint8_t>& data) {
  if (pfd.size() < 0x78 || ld_be(pfd.data() + 4) != 0x50464442u) return false;
  const uint64_t capacity = ld_be(pfd.data() + 0x64), used = ld_be(pfd.data() + 0x74);
  const size_t table = 0x78 + size_t(capacity) * 8;
  for (uint64_t i = 0; i < used; ++i) {
    const uint8_t* e = pfd.data() + table + size_t(i) * 272;
    if (e + 272 > pfd.data() + pfd.size()) return false;
    if (strnlen(reinterpret_cast<const char*>(e + 8), 65) != file_name.size() ||
        std::memcmp(e + 8, file_name.data(), file_name.size()) != 0)
      continue;
    const uint64_t size = uint64_t(ld_be(e + 264)) << 32 | ld_be(e + 268);
    if (size > data.size()) return false;
    // The entry key: AES-CBC with the syscon manager key, the IV made from the secure file id.
    uint8_t iv[16];
    for (int k = 0, j = 0; k < 16; ++k) iv[k] = k == 1 ? 11 : k == 2 ? 15 : k == 5 ? 14 : k == 8 ? 10 : kSecureFileId[j++];
    uint8_t key[16];
    std::memcpy(key, e + 80, 16);
    Aes128(kSysconManagerKey).Decrypt(key);
    for (int k = 0; k < 16; ++k) key[k] ^= iv[k];
    // The file: per 16-byte block, AES decrypt, XOR the encrypted block counter.
    const Aes128 aes(key);
    data.resize((data.size() + 15) & ~size_t(15));
    for (size_t b = 0; b < data.size() / 16; ++b) {
      uint8_t counter[16] = {};
      for (int k = 0; k < 8; ++k) counter[k] = uint8_t(uint64_t(b) >> (56 - 8 * k));
      aes.Encrypt(counter);
      aes.Decrypt(data.data() + b * 16);
      for (int k = 0; k < 16; ++k) data[b * 16 + k] ^= counter[k];
    }
    data.resize(size_t(size));
    return true;
  }
  return false;
}

inline uint64_t SaveKey(const std::vector<uint8_t>& key_dat) {
  uint64_t k = 0;
  for (int i = 0; i < 8; ++i) k = k << 8 | uint8_t(key_dat[i] ^ key_dat[8 + i]);
  return lr_steam_saves::SetupKey((k | 1) ^ kGameKey);
}

// The plain body, or false.
inline bool DecryptApp(const std::vector<uint8_t>& app, const std::vector<uint8_t>& key_dat, std::vector<uint8_t>& body) {
  if (app.size() != kPs3Size || key_dat.size() != 16) return false;
  body = app;
  lr_steam_saves::Crypt(body.data(), body.size(), SaveKey(key_dat), true);
  return std::memcmp(body.data(), "db_partyman\0", 12) == 0 &&
         ld_le(body.data() + body.size() - 4) == lr_steam_saves::Checksum(body.data(), body.size());
}

// PARAM.SFO DETAIL ("Total hours played: 012:34:56"), or empty.
inline std::string SfoDetail(const std::vector<uint8_t>& sfo) {
  if (sfo.size() < 20 || ld_be(sfo.data()) != 0x00505346u) return {};
  const uint32_t keys = ld_le(sfo.data() + 8), values = ld_le(sfo.data() + 12), count = ld_le(sfo.data() + 16);
  for (uint32_t i = 0; i < count && 20 + (i + 1) * 16 <= sfo.size(); ++i) {
    const uint8_t* e = sfo.data() + 20 + i * 16;
    const size_t key = keys + (e[0] | e[1] << 8), len = ld_le(e + 4), value = values + ld_le(e + 12);
    if (key + 7 > sfo.size() || std::memcmp(sfo.data() + key, "DETAIL", 7) != 0 || value + len > sfo.size()) continue;
    return std::string(reinterpret_cast<const char*>(sfo.data() + value), strnlen(reinterpret_cast<const char*>(sfo.data() + value), len));
  }
  return {};
}

// A PS3 save folder's files -> 360 APP.DAT. False (and why) when it is not a Lightning Returns save.
inline bool Ps3ToXbox(const std::vector<uint8_t>& app_in, const std::vector<uint8_t>& key_in,
                      const std::vector<uint8_t>& pfd, const std::string& detail, std::vector<uint8_t>& out,
                      std::string& why) {
  std::vector<uint8_t> body;
  bool ok = DecryptApp(app_in, key_in, body);
  if (!ok && !pfd.empty()) {
    std::vector<uint8_t> app = app_in, key = key_in;
    ok = PfdDecrypt(pfd, "APP.DAT", app) && PfdDecrypt(pfd, "KEY.DAT", key) && DecryptApp(app, key, body);
  }
  if (!ok) {
    why = app_in.size() != kPs3Size ? "not a Lightning Returns PS3 save (APP.DAT size)"
                                    : "does not decrypt (damaged, or KEY.DAT / PARAM.PFD missing or from another save)";
    return false;
  }
  using lr_steam_saves::kSections;
  std::vector<uint8_t> xb(body.begin(), body.begin() + kSections * 32);
  size_t from = kSections * 32;
  std::vector<std::pair<size_t, size_t>> data;  // offset, size to copy
  int resized = 0;
  for (int s = 0; s < kSections; ++s) {
    uint8_t* e = xb.data() + s * 32;
    const uint32_t size = ld_be(e + 28);
    if (from + size > body.size() - 8) {
      why = "section table out of range";
      return false;
    }
    uint32_t copy = size;
    if (s == 0) {
      if (std::memcmp(e, "db_partyman\0", 12) != 0 || ld_be(e + 16) != kPs3RootId) {
        why = "unexpected save layout (root id)";
        return false;
      }
      st_be(e + 16, lr_steam_saves::kXboxRootId);
    }
    for (const Resize& r : kResized) {
      if (std::strncmp(reinterpret_cast<const char*>(e), r.name, 16) != 0) continue;
      if (size != r.ps3) {
        why = std::string("unexpected save layout (") + r.name + " size)";
        return false;
      }
      for (size_t i = from + r.xbox; i < from + r.ps3; ++i) {
        if (body[i]) {
          why = std::string("PS3-only data in ") + r.name + " (not converted)";
          return false;
        }
      }
      copy = r.xbox;
      st_be(e + 28, copy);
      ++resized;
    }
    data.emplace_back(from, copy);
    from += size;
  }
  if (resized != int(std::size(kResized))) {
    why = "unexpected save layout";
    return false;
  }
  for (size_t s = 0; s < data.size(); ++s) {
    const auto& [offset, size] = data[s];
    const size_t at = xb.size();
    xb.insert(xb.end(), body.begin() + offset, body.begin() + offset + size);
    if (lr_steam_saves::FixEquipmentRefs(xb.data() + at, size, xb.data() + s * 32))
      REXLOG_WARN("LR PS3 saves: invalid equipment references in {} cleared",
                  reinterpret_cast<const char*>(xb.data() + s * 32));
  }
  xb.insert(xb.end(), body.begin() + from, body.end() - 8);  // padding after the data
  xb.resize(xb.size() + 8);
  if (xb.size() != lr_steam_saves::kXboxSize - lr_steam_saves::kXboxHeader) {
    why = "unexpected save size";
    return false;
  }
  st_le(xb.data() + xb.size() - 8, uint32_t(xb.size() - 8));
  st_le(xb.data() + xb.size() - 4, lr_steam_saves::Checksum(xb.data(), xb.size()));
  const uint64_t key = lr_steam_saves::SetupKey(lr_steam_saves::kXboxSeed);
  lr_steam_saves::Crypt(xb.data(), xb.size(), key, false);

  const uint64_t clock = uint64_t(ld_be(body.data() + 0x136A)) << 32 | ld_be(body.data() + 0x136E);
  const uint64_t day = std::min<uint64_t>(clock / kClockPerSecond / 86400, 0xFF);
  out.assign(lr_steam_saves::kXboxHeader, 0);
  out[0] = 0x0D;
  const uint64_t flags = lr_steam_saves::kXboxFlagsBase + (day << 4);
  for (int i = 0; i < 8; ++i) out[8 + i] = uint8_t(flags >> (56 - 8 * i));
  std::memcpy(out.data() + 0x10, detail.data(), std::min<size_t>(detail.size(), 0xFF));
  out.insert(out.end(), xb.begin(), xb.end());

  // Check the result the way the game reads it.
  std::vector<uint8_t> check(out.begin() + lr_steam_saves::kXboxHeader, out.end());
  lr_steam_saves::Crypt(check.data(), check.size(), key, true);
  if (std::memcmp(check.data(), "db_partyman\0", 12) != 0 ||
      ld_le(check.data() + check.size() - 4) != lr_steam_saves::Checksum(check.data(), check.size())) {
    why = "conversion self-check failed";
    return false;
  }
  return true;
}

inline std::vector<uint8_t> ReadAll(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// settings_dir: the user data folder (saves under B13EBABEBABEBABE\5351081B).
inline void Import(const std::filesystem::path& settings_dir) {
  std::error_code ec;
  const std::filesystem::path source = settings_dir / "ps3_saves";
  if (!std::filesystem::is_directory(source, ec)) return;
  const std::filesystem::path saves = settings_dir / "B13EBABEBABEBABE" / "5351081B" / "00000001";
  const std::filesystem::path headers = settings_dir / "B13EBABEBABEBABE" / "5351081B" / "Headers" / "00000001";
  const std::filesystem::path list_path = settings_dir / "ps3_saves_imported.txt";
  std::set<std::string> imported;
  {
    std::ifstream list(list_path);
    std::string line;
    while (std::getline(list, line))
      if (line.size() >= 16) imported.insert(line.substr(0, 16));
  }
  auto slot_used = [&](int slot) {
    char name[32];
    std::snprintf(name, sizeof(name), "ff13-%02d.dat", slot);
    return std::filesystem::exists(saves / name, ec) || std::filesystem::exists(headers / (std::string(name) + ".header"), ec);
  };
  // Save folders anywhere under ps3_saves (a whole savedata folder can be dropped in).
  std::vector<std::filesystem::path> folders;
  for (auto it = std::filesystem::recursive_directory_iterator(source, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file(ec) && it->path().filename() == "APP.DAT") folders.push_back(it->path().parent_path());
  std::sort(folders.begin(), folders.end());
  int added = 0;
  for (const auto& dir : folders) {
    const std::vector<uint8_t> app = ReadAll(dir / "APP.DAT"), key = ReadAll(dir / "KEY.DAT");
    std::vector<uint8_t> both = app;
    both.insert(both.end(), key.begin(), key.end());
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llX", static_cast<unsigned long long>(lr_steam_saves::Fnv64(both)));
    if (imported.count(hash)) continue;
    std::vector<uint8_t> xbox;
    std::string why;
    if (!Ps3ToXbox(app, key, ReadAll(dir / "PARAM.PFD"), SfoDetail(ReadAll(dir / "PARAM.SFO")), xbox, why)) {
      REXLOG_WARN("LR PS3 saves: {} not imported: {}", dir.string(), why);
      continue;
    }
    std::string removed;
    if (lr_steam_saves::RemoveUnavailable(xbox, lr_steam_saves::ReadUnavailable(settings_dir), removed))
      REXLOG_INFO("LR PS3 saves: {}: DLC items the 360 game does not have removed: {}", dir.string(), removed);
    // The PS3 slot number: the folder name ends in -NN.
    const std::string folder = dir.filename().string();
    int slot = -1;
    if (folder.size() > 3 && folder[folder.size() - 3] == '-') std::sscanf(folder.c_str() + folder.size() - 2, "%d", &slot);
    if (slot < 0 || slot > lr_steam_saves::kMaxSlot || slot_used(slot)) {
      slot = -1;
      for (int s = 0; s <= lr_steam_saves::kMaxSlot && slot < 0; ++s)
        if (!slot_used(s)) slot = s;
    }
    if (slot < 0) {
      REXLOG_WARN("LR PS3 saves: {} not imported: no free save slot", dir.string());
      continue;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "ff13-%02d.dat", slot);
    std::filesystem::create_directories(saves / name, ec);
    std::filesystem::create_directories(headers, ec);
    const std::vector<uint8_t> header = lr_steam_saves::ContentHeader(name, slot);
    std::ofstream out_app(saves / name / "APP.DAT", std::ios::binary);
    out_app.write(reinterpret_cast<const char*>(xbox.data()), std::streamsize(xbox.size()));
    std::ofstream head(headers / (std::string(name) + ".header"), std::ios::binary);
    head.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    if (!out_app.good() || !head.good()) {
      REXLOG_WARN("LR PS3 saves: could not write slot {}", name);
      continue;
    }
    std::ofstream list(list_path, std::ios::app);
    list << hash << ' ' << name << ' ' << dir.string() << '\n';
    imported.insert(hash);
    ++added;
    REXLOG_INFO("LR PS3 saves: {} -> {} (Save Data {:02d}, {})", dir.string(), name, slot + 1,
                std::string(reinterpret_cast<const char*>(xbox.data()) + 0x10,
                            strnlen(reinterpret_cast<const char*>(xbox.data()) + 0x10, 0x100)));
  }
  if (!folders.empty()) REXLOG_INFO("LR PS3 saves: {} found, {} newly imported", folders.size(), added);
}

}  // namespace lr_ps3_saves
