// Lightning Returns port - Japanese voice patch builder.
//
// Reads the English voice files of the Xbox 360 release and the Japanese voice files of the
// Steam release (same file codes on both platforms), converts every Japanese voice file to the
// 360 layout and writes them as a 360 update patch:
//   <out>/filelist_patch_u.x360.bin  (plain file list)
//   <out>/white_patch_u.x360.bin     (stored files)
// The game reads that folder as UPDATE:\ (--update_data_root) and then loads the patched
// files instead of the ones in its own archives. The 360 and Steam files are only read.
//
//   lr_jpvoice [--ffmpeg <ffmpeg>] <360 game folder> <Steam game folder> <out folder> [samples folder]
//   lr_jpvoice --scd <360 english.scd> <steam japanese.scd> <out.scd>
//   lr_jpvoice --wpd <360 english.wpd> <steam japanese.wpd> <out.wpd>
//   lr_jpvoice --lipsync <360 file> <steam english file> <steam japanese file> <out file>
//   lr_jpvoice [--ffmpeg <ffmpeg>] --dlc <patch folder> <Steam game folder> <content folder>...
//   lr_jpvoice --files <patch folder> <360 game folder> <file>...   (mods: replace game files by name)
//   lr_jpvoice --remove-files <patch folder> <360 game folder> <file name>...
//   lr_jpvoice --list <filelist.bin>
//   lr_jpvoice --extract <360 game folder> <Steam game folder> <out folder> <path part>...
//
// SCD: "SEDBSSCF" sound container. The 360 files are big-endian, the Steam ones little-endian;
// both hold Ogg Vorbis voices (codec 6). The converted file keeps the 360 English file's header
// and sound tables and takes the Japanese stream headers (byte-swapped) and Ogg data.
// WPD: simple bundle ("WPD", count, 32-byte entries) of SCDs, converted member by member.
// The same patch also gets the Japanese lip-sync and facial motion (see merge_jp_motion), so
// Settings > Voice switches voices and mouths together.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
typedef std::vector<uint8_t> Bytes;

static uint32_t rd32(const uint8_t* p, bool be) {
  return be ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3])
            : (uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]);
}
static uint16_t rd16(const uint8_t* p, bool be) {
  return be ? uint16_t(p[0] << 8 | p[1]) : uint16_t(p[1] << 8 | p[0]);
}
static void wr32(uint8_t* p, uint32_t v, bool be) {
  for (int i = 0; i < 4; i++) p[be ? i : 3 - i] = uint8_t(v >> (24 - 8 * i));
}
static void wr16(uint8_t* p, uint16_t v, bool be) {
  p[be ? 0 : 1] = uint8_t(v >> 8);
  p[be ? 1 : 0] = uint8_t(v);
}
static void swap32(uint8_t* p) { std::swap(p[0], p[3]); std::swap(p[1], p[2]); }
static size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

static Bytes read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + p.string());
  return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void write_file(const fs::path& p, const Bytes& d) {
  std::ofstream f(p, std::ios::binary);
  f.write((const char*)d.data(), d.size());
  if (!f) throw std::runtime_error("cannot write " + p.string());
}

// ---------------------------------------------------------------- inflate (RFC 1950/1951)

namespace inflate {
struct State {
  const uint8_t* in;
  size_t n, pos = 0;
  uint32_t buf = 0;
  int cnt = 0;
  Bytes* out;
  int bits(int need) {
    uint32_t v = buf;
    while (cnt < need) {
      if (pos >= n) throw std::runtime_error("inflate: out of input");
      v |= uint32_t(in[pos++]) << cnt;
      cnt += 8;
    }
    buf = v >> need;
    cnt -= need;
    return int(v & ((1u << need) - 1));
  }
};
struct Huff {
  uint16_t count[16];
  uint16_t sym[320];
};
static void build(Huff& h, const uint8_t* len, int n) {
  memset(h.count, 0, sizeof(h.count));
  for (int i = 0; i < n; i++) h.count[len[i]]++;
  h.count[0] = 0;
  uint16_t offs[16];
  offs[1] = 0;
  for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h.count[l];
  for (int i = 0; i < n; i++)
    if (len[i]) h.sym[offs[len[i]]++] = uint16_t(i);
}
static int decode(State& s, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; len++) {
    code |= s.bits(1);
    int count = h.count[len];
    if (code - count < first) return h.sym[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  throw std::runtime_error("inflate: bad code");
}
static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                   193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                   6145, 8193, 12289, 16385, 24577};
static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static void codes(State& s, const Huff& lc, const Huff& dc) {
  for (;;) {
    int sym = decode(s, lc);
    if (sym < 256) {
      s.out->push_back(uint8_t(sym));
    } else if (sym == 256) {
      return;
    } else {
      sym -= 257;
      if (sym >= 29) throw std::runtime_error("inflate: bad length");
      size_t len = lbase[sym] + s.bits(lext[sym]);
      int ds = decode(s, dc);
      if (ds >= 30) throw std::runtime_error("inflate: bad distance");
      size_t dist = dbase[ds] + s.bits(dext[ds]);
      if (dist > s.out->size()) throw std::runtime_error("inflate: distance too far");
      size_t from = s.out->size() - dist;
      for (size_t i = 0; i < len; i++) s.out->push_back((*s.out)[from + i]);
    }
  }
}
// zlib stream -> appended to out
static void zlib(const uint8_t* in, size_t n, Bytes& out) {
  if (n < 2 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31)
    throw std::runtime_error("inflate: not zlib data");
  State s;
  s.in = in + 2;
  s.n = n - 2;
  s.out = &out;
  int last;
  do {
    last = s.bits(1);
    int type = s.bits(2);
    if (type == 0) {
      s.buf = 0;
      s.cnt = 0;
      if (s.pos + 4 > s.n) throw std::runtime_error("inflate: out of input");
      unsigned len = s.in[s.pos] | s.in[s.pos + 1] << 8;
      s.pos += 4;
      if (s.pos + len > s.n) throw std::runtime_error("inflate: out of input");
      out.insert(out.end(), s.in + s.pos, s.in + s.pos + len);
      s.pos += len;
    } else if (type == 1) {
      static Huff lc, dc;
      static bool init = false;
      if (!init) {
        uint8_t l[288];
        for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        build(lc, l, 288);
        for (int i = 0; i < 30; i++) l[i] = 5;
        build(dc, l, 30);
        init = true;
      }
      codes(s, lc, dc);
    } else if (type == 2) {
      int nlen = s.bits(5) + 257, ndist = s.bits(5) + 1, ncode = s.bits(4) + 4;
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                        11, 4, 12, 3, 13, 2, 14, 1, 15};
      uint8_t l[320] = {0};
      for (int i = 0; i < ncode; i++) l[order[i]] = uint8_t(s.bits(3));
      Huff lc, dc;
      build(lc, l, 19);
      int i = 0;
      uint8_t lens[320] = {0};
      while (i < nlen + ndist) {
        int sym = decode(s, lc);
        if (sym < 16) {
          lens[i++] = uint8_t(sym);
        } else {
          int rep, val = 0;
          if (sym == 16) {
            if (!i) throw std::runtime_error("inflate: bad repeat");
            val = lens[i - 1];
            rep = 3 + s.bits(2);
          } else if (sym == 17) {
            rep = 3 + s.bits(3);
          } else {
            rep = 11 + s.bits(7);
          }
          if (i + rep > nlen + ndist) throw std::runtime_error("inflate: bad lengths");
          while (rep--) lens[i++] = uint8_t(val);
        }
      }
      build(lc, lens, nlen);
      build(dc, lens + nlen, ndist);
      codes(s, lc, dc);
    } else {
      throw std::runtime_error("inflate: bad block type");
    }
  } while (!last);
}
}  // namespace inflate

// zlib stream made of stored blocks (what the file list chunks need; no compression needed)
static Bytes zlib_stored(const Bytes& d) {
  Bytes z = {0x78, 0x01};
  size_t pos = 0;
  do {
    size_t n = std::min<size_t>(65535, d.size() - pos);
    z.push_back(pos + n == d.size() ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), d.begin() + pos, d.begin() + pos + n);
    pos += n;
  } while (pos < d.size());
  uint32_t a = 1, b = 0;
  for (uint8_t c : d) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  uint32_t adler = b << 16 | a;
  for (int i = 3; i >= 0; i--) z.push_back(uint8_t(adler >> (8 * i)));
  return z;
}

// ---------------------------------------------------------------- file lists
// Port of the file list crypto from Surihix's WhiteBinTools (GPL-3.0).

struct Entry {
  uint32_t code = 0;
  uint8_t ftype = 0;
  uint64_t sector = 0;
  uint32_t usize = 0, csize = 0;
  std::string path;
};

static void xor_table(uint64_t seed64, uint8_t table[264]) {
  uint8_t seed[8];
  for (int i = 0; i < 8; i++) seed[7 - i] = uint8_t(seed64 >> (8 * i));  // reversed LE bytes
  uint32_t a = rd32(seed, false), b = rd32(seed + 4, false);
  a = (a << 8) | (a >> 24);
  b = (b >> 16) | (b << 16);
  uint8_t blk[8];
  wr32(blk, b, false);
  wr32(blk + 4, a, false);
  blk[0] = uint8_t(blk[0] + 0x45);
  for (int i = 1; i < 8; i++) {
    unsigned t = blk[i] + 0xD4 + blk[i - 1];
    t ^= unsigned(blk[i - 1]) << 2;
    t ^= 0x45;
    blk[i] = uint8_t(t);
  }
  memcpy(table, blk, 8);
  uint64_t prev = uint64_t(rd32(blk, false)) | uint64_t(rd32(blk + 4, false)) << 32;
  for (int i = 1; i < 0x21; i++) {
    uint32_t ha = uint32_t(prev), hb = uint32_t(prev >> 32);
    uint64_t x = prev * 5;
    x ^= uint64_t(hb) << 32;
    uint32_t ta = ha ^ uint32_t(x);
    uint32_t tb = uint32_t(x >> 32);
    x = ha | (x & 0xFFFFFFFF00000000ull);
    uint32_t xa = uint32_t(x) ^ ta;
    uint32_t xb = tb ^ hb;
    wr32(table + i * 8, xa, false);
    wr32(table + i * 8 + 4, xb, false);
    prev = uint64_t(xa) | uint64_t(xb) << 32;
  }
}

static Bytes decrypt_filelist(const Bytes& data) {
  if (data.size() < 32) throw std::runtime_error("file list too small");
  uint32_t body = rd32(&data[16], true) + 8;
  const uint8_t* s = data.data();
  uint32_t seed32 = uint32_t(s[9]) << 24 | uint32_t(s[12]) << 16 | uint32_t(s[2]) << 8 | s[0];
  uint64_t seed = seed32;
  if (seed32 & 0x80000000u) seed |= 0xFFFFFFFF00000000ull;
  uint8_t table[264];
  xor_table(seed, table);
  Bytes out = data;
  size_t pos = 32;
  if (32 + size_t(body) > data.size()) throw std::runtime_error("file list truncated");
  for (uint32_t n = 0; n < body / 8; n++, pos += 8) {
    uint32_t counter = n * 8;
    const uint8_t* cur = &data[pos];
    unsigned off = counter & 0xF8;
    auto loop = [&](unsigned v) {
      for (int k = 0; k < 8; k++) v = (((v + 120) & 0xFF) - table[off + k]) & 0xFF;
      return uint8_t(v);
    };
    uint8_t d[8];
    d[0] = loop(((n ^ 69) & 255) ^ cur[0]);
    for (int k = 1; k < 8; k++) d[k] = loop(cur[k - 1] ^ cur[k]);
    uint8_t arr[8] = {d[4], d[5], d[6], d[7], d[0], d[1], d[2], d[3]};
    uint32_t hi = rd32(arr, false), lo = rd32(arr + 4, false);
    uint32_t xlo = rd32(table + off, false), xhi = rd32(table + off + 4, false);
    uint64_t c = counter;
    uint64_t ab = c << 10, cd = c << 20, ef = c << 30;
    uint64_t cv = (cd & 0xFFFFFFFF) | (ab & 0xFFFFFFFF);
    uint64_t dv = (cd >> 32) | (ab >> 32);
    uint32_t ev = uint32_t((ef & 0xFFFFFFFF) | c | cv);
    uint32_t fv = uint32_t((ef >> 32) | dv);
    uint32_t carry1 = ev > uint32_t(~0xA1652347u) ? 1 : 0;
    uint32_t k1 = ev + 0xA1652347u;
    uint32_t k2 = fv + carry1;
    uint32_t L = lo - xlo;
    uint32_t H = hi - xhi - (lo < xlo ? 1 : 0);
    L ^= k1;
    H ^= k2;
    L ^= xlo;
    H ^= xhi;
    wr32(&out[pos], H, false);
    wr32(&out[pos + 4], L, false);
  }
  uint32_t size = rd32(&out[16], true);
  if (32 + size_t(size) + 8 > out.size()) throw std::runtime_error("file list size field");
  uint32_t chk = rd32(&out[32 + size + 4], false), calc = 0;
  for (uint32_t i = 0; i < size / 4; i++) calc += out[32 + 4 * i];
  if (chk != calc) throw std::runtime_error("file list checksum mismatch after decrypt");
  return out;
}

static std::vector<Entry> parse_filelist(Bytes raw) {
  size_t base = 0;
  if (raw.size() >= 24 && rd32(&raw[20], false) == 0x1DE03478) {
    raw = decrypt_filelist(raw);
    base = 32;
  }
  if (raw.size() < base + 12) throw std::runtime_error("file list too small");
  bool be = rd32(&raw[base + 8], false) >= 0x100000;
  uint32_t chunk_tab = rd32(&raw[base], be), chunk_data = rd32(&raw[base + 4], be),
           count = rd32(&raw[base + 8], be);
  if (base + 12 + size_t(count) * 8 > raw.size()) throw std::runtime_error("file list entries");
  struct Raw { uint32_t code; unsigned info, chunk; uint8_t ftype; };
  std::vector<Raw> ents(count);
  unsigned nchunks = 0;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t* p = &raw[base + 12 + i * 8];
    Raw r;
    r.code = rd32(p, be);
    unsigned info = rd16(p + 4, be);
    r.chunk = unsigned(p[6]) << 1 | info >> 15;
    r.info = info & 0x7FFF;
    r.ftype = p[7];
    ents[i] = r;
    nchunks = std::max(nchunks, r.chunk + 1);
  }
  std::vector<Bytes> chunks(nchunks);
  for (unsigned c = 0; c < nchunks; c++) {
    size_t t = base + chunk_tab + 12 * c;
    if (t + 12 > raw.size()) throw std::runtime_error("file list chunk table");
    uint32_t un = rd32(&raw[t], be), cm = rd32(&raw[t + 4], be), cp = rd32(&raw[t + 8], be);
    size_t p = base + chunk_data + cp;
    if (p + cm > raw.size()) throw std::runtime_error("file list chunk data");
    inflate::zlib(&raw[p], cm, chunks[c]);
    if (chunks[c].size() != un) throw std::runtime_error("file list chunk size");
  }
  std::vector<Entry> out;
  out.reserve(count);
  for (auto& r : ents) {
    const Bytes& b = chunks[r.chunk];
    if (r.info >= b.size()) throw std::runtime_error("file list string offset");
    std::string text((const char*)&b[r.info]);
    Entry e;
    e.code = r.code;
    e.ftype = r.ftype;
    unsigned long long sec = 0, us = 0, cs = 0;
    char path[1024] = {0};
    if (sscanf(text.c_str(), "%llx:%llx:%llx:%1023s", &sec, &us, &cs, path) != 4)
      throw std::runtime_error("file list entry text: " + text);
    e.sector = sec;
    e.usize = uint32_t(us);
    e.csize = uint32_t(cs);
    e.path = path;
    out.push_back(e);
  }
  return out;
}

// ---------------------------------------------------------------- archives

struct Archive {
  fs::path path;
  std::ifstream f;
  uint64_t size = 0;
  explicit Archive(const fs::path& p) : path(p), f(p, std::ios::binary) {
    if (!f) throw std::runtime_error("cannot open " + p.string());
    size = fs::file_size(p);
  }
  Bytes raw(uint64_t offset, uint32_t n) {
    if (offset + n > size) throw std::runtime_error("entry past the end of " + path.filename().string());
    Bytes b(n);
    f.clear();
    f.seekg(std::streamoff(offset));
    f.read((char*)b.data(), n);
    if (!f) throw std::runtime_error("read failed in " + path.filename().string());
    return b;
  }
  Bytes read(const Entry& e) {
    Bytes b = raw(e.sector * 2048, e.csize);
    if (e.csize == e.usize) return b;
    Bytes out;
    out.reserve(e.usize);
    inflate::zlib(b.data(), b.size(), out);
    if (out.size() != e.usize) throw std::runtime_error("unexpected size after inflate: " + e.path);
    return out;
  }
};

// A file list together with the archive it describes.
struct Source {
  std::string name;
  std::shared_ptr<Archive> archive;
  std::vector<Entry> entries;
};

// ---------------------------------------------------------------- SCD

struct Scd {
  bool be = true;
  uint32_t tables = 0, headers_offset = 0;
  std::vector<uint32_t> metas;  // table 3 (stream headers), in table order
};

static bool parse_scd(const Bytes& d, Scd& s, std::string& why) {
  if (d.size() < 0x50 || memcmp(d.data(), "SEDBSSCF", 8)) { why = "not an SCD"; return false; }
  s.be = d[0x0C] == 1;
  uint32_t version = rd32(&d[8], s.be);
  if (version != 3) { why = "SCD version " + std::to_string(version); return false; }
  s.tables = rd16(&d[0x0E], s.be);
  if (s.tables + 0x20 > d.size()) { why = "SCD tables"; return false; }
  unsigned n = rd16(&d[s.tables + 4], s.be);
  s.headers_offset = rd32(&d[s.tables + 0x0C], s.be);
  if (s.headers_offset + 4ull * n > d.size()) { why = "SCD header table"; return false; }
  s.metas.clear();
  for (unsigned i = 0; i < n; i++) {
    uint32_t m = rd32(&d[s.headers_offset + 4 * i], s.be);
    if (m + 0x20ull > d.size()) { why = "SCD stream header offset"; return false; }
    s.metas.push_back(m);
  }
  return true;
}

static const uint8_t kOggV3Table[256] = {
    0x3A, 0x32, 0x32, 0x32, 0x03, 0x7E, 0x12, 0xF7, 0xB2, 0xE2, 0xA2, 0x67, 0x32, 0x32, 0x22, 0x32,
    0x32, 0x52, 0x16, 0x1B, 0x3C, 0xA1, 0x54, 0x7B, 0x1B, 0x97, 0xA6, 0x93, 0x1A, 0x4B, 0xAA, 0xA6,
    0x7A, 0x7B, 0x1B, 0x97, 0xA6, 0xF7, 0x02, 0xBB, 0xAA, 0xA6, 0xBB, 0xF7, 0x2A, 0x51, 0xBE, 0x03,
    0xF4, 0x2A, 0x51, 0xBE, 0x03, 0xF4, 0x2A, 0x51, 0xBE, 0x12, 0x06, 0x56, 0x27, 0x32, 0x32, 0x36,
    0x32, 0xB2, 0x1A, 0x3B, 0xBC, 0x91, 0xD4, 0x7B, 0x58, 0xFC, 0x0B, 0x55, 0x2A, 0x15, 0xBC, 0x40,
    0x92, 0x0B, 0x5B, 0x7C, 0x0A, 0x95, 0x12, 0x35, 0xB8, 0x63, 0xD2, 0x0B, 0x3B, 0xF0, 0xC7, 0x14,
    0x51, 0x5C, 0x94, 0x86, 0x94, 0x59, 0x5C, 0xFC, 0x1B, 0x17, 0x3A, 0x3F, 0x6B, 0x37, 0x32, 0x32,
    0x30, 0x32, 0x72, 0x7A, 0x13, 0xB7, 0x26, 0x60, 0x7A, 0x13, 0xB7, 0x26, 0x50, 0xBA, 0x13, 0xB4,
    0x2A, 0x50, 0xBA, 0x13, 0xB5, 0x2E, 0x40, 0xFA, 0x13, 0x95, 0xAE, 0x40, 0x38, 0x18, 0x9A, 0x92,
    0xB0, 0x38, 0x00, 0xFA, 0x12, 0xB1, 0x7E, 0x00, 0xDB, 0x96, 0xA1, 0x7C, 0x08, 0xDB, 0x9A, 0x91,
    0xBC, 0x08, 0xD8, 0x1A, 0x86, 0xE2, 0x70, 0x39, 0x1F, 0x86, 0xE0, 0x78, 0x7E, 0x03, 0xE7, 0x64,
    0x51, 0x9C, 0x8F, 0x34, 0x6F, 0x4E, 0x41, 0xFC, 0x0B, 0xD5, 0xAE, 0x41, 0xFC, 0x0B, 0xD5, 0xAE,
    0x41, 0xFC, 0x3B, 0x70, 0x71, 0x64, 0x33, 0x32, 0x12, 0x32, 0x32, 0x36, 0x70, 0x34, 0x2B, 0x56,
    0x22, 0x70, 0x3A, 0x13, 0xB7, 0x26, 0x60, 0xBA, 0x1B, 0x94, 0xAA, 0x40, 0x38, 0x00, 0xFA, 0xB2,
    0xE2, 0xA2, 0x67, 0x32, 0x32, 0x12, 0x32, 0xB2, 0x32, 0x32, 0x32, 0x32, 0x75, 0xA3, 0x26, 0x7B,
    0x83, 0x26, 0xF9, 0x83, 0x2E, 0xFF, 0xE3, 0x16, 0x7D, 0xC0, 0x1E, 0x63, 0x21, 0x07, 0xE3, 0x01,
};

// Checks one stream block (stream header at b[0]) and returns the Ogg layout. Works on either
// byte order. For the 360 files: header XOR byte applies to the Vorbis header pages only.
struct OggInfo {
  uint32_t codec = 0, stream_size = 0, extradata = 0, aux = 0;
  size_t mini = 0, seek = 0, vorb = 0;  // offsets/sizes inside the block
  uint8_t version = 0, xorb = 0;
};
static bool check_stream(const uint8_t* b, size_t len, bool be, OggInfo& o, std::string& why) {
  if (len < 0x20) { why = "stream header past the end"; return false; }
  o.stream_size = rd32(b, be);
  o.codec = rd32(b + 0x0C, be);
  o.extradata = rd32(b + 0x18, be);
  o.aux = rd32(b + 0x1C, be);
  if (o.codec == 0xFFFFFFFF) return true;  // dummy entry
  if (o.codec != 6) { why = "codec " + std::to_string(o.codec) + " (not Ogg)"; return false; }
  if (0x20ull + o.extradata + o.stream_size > len) { why = "stream data past the end"; return false; }
  size_t p = 0x20;
  if (o.aux) {
    if (o.aux != 1 || memcmp(b + p, "MARK", 4)) { why = "unknown aux chunk"; return false; }
    uint32_t asz = rd32(b + p + 4, be);
    if (asz < 8 || asz % 4 || p + asz + 0x20 > len) { why = "MARK chunk size"; return false; }
    p += asz;
  }
  o.mini = p;
  o.version = b[p];
  o.xorb = b[p + 2];
  if (o.version != 2 && o.version != 3) { why = "Ogg header version " + std::to_string(o.version); return false; }
  o.seek = rd32(b + p + 0x10, be);
  o.vorb = rd32(b + p + 0x14, be);
  if ((p - 0x20) + 0x20 + o.seek + o.vorb != o.extradata) { why = "Ogg header sizes"; return false; }
  size_t vstart = p + 0x20 + o.seek;
  const uint8_t* v = b + vstart;
  uint8_t x = o.version == 2 ? o.xorb : 0;
  if (o.version == 2 && ((v[0] ^ x) != 'O' || (v[1] ^ x) != 'g' || (v[2] ^ x) != 'g' || (v[3] ^ x) != 'S')) {
    why = "Vorbis header does not decode to an Ogg page";
    return false;
  }
  if (o.version == 2 && o.stream_size >= 4 && memcmp(v + o.vorb, "OggS", 4)) {
    why = "audio data does not start with an Ogg page";
    return false;
  }
  return true;
}

// Byte-swaps a little-endian stream block (header, MARK chunk, Ogg header, seek table) to
// big-endian in place; turns a version 3 (table XOR) Ogg stream into a version 2 one.
static bool block_to_be(uint8_t* b, size_t len, std::string& why) {
  OggInfo o;
  if (!check_stream(b, len, false, o, why)) return false;
  for (int i = 0; i < 8; i++) swap32(b + 4 * i);
  if (o.codec == 0xFFFFFFFF) return true;
  if (o.aux)
    for (size_t p = 0x24, end = 0x20 + rd32(b + 0x24, true); p < end; p += 4) swap32(b + p);
  // version 3 decryption works on the little-endian sizes read above
  size_t vstart = o.mini + 0x20 + o.seek;
  if (o.version == 3) {
    uint8_t key = uint8_t(o.stream_size & 0xFF);
    uint8_t b1 = key & 0x7F, b2 = key & 0x3F;
    for (size_t i = 0; i < o.vorb + o.stream_size; i++)
      b[vstart + i] ^= kOggV3Table[(b2 + i) & 0xFF] ^ b1;
    uint8_t x = 0x1E;  // what the 360 files use
    for (size_t i = 0; i < o.vorb; i++) b[vstart + i] ^= x;
    b[o.mini] = 2;
    b[o.mini + 2] = x;
  }
  for (int i = 1; i < 8; i++) swap32(b + o.mini + 4 * i);
  for (size_t i = 0; i + 4 <= o.seek; i += 4) swap32(b + o.mini + 0x20 + i);
  OggInfo check;
  if (!check_stream(b, len, true, check, why)) { why = "after conversion: " + why; return false; }
  return true;
}

// ---------------------------------------------------------------- MS ADPCM -> Ogg Vorbis
// Most Steam voices are MS ADPCM (codec 12), which the 360 sound driver does not play. They
// are decoded here and re-encoded to Ogg Vorbis by ffmpeg (g_ffmpeg), then wrapped the way
// the 360 files are: Ogg header (version 2, header pages XOR'ed with one byte), a seek table
// with one entry per 0.1 s (byte offset of the page holding that sample) and the audio pages.

static std::string g_ffmpeg;  // set from the command line; empty: MS ADPCM files stay English
static fs::path g_tmp;
static std::atomic<unsigned> g_tmp_counter{0};

static bool decode_msadpcm(const uint8_t* d, size_t n, int ch, int align, const int16_t* coef, int ncoef,
                           std::vector<int16_t>& out) {
  static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
  if (ch < 1 || ch > 2 || align < 7 * ch) return false;
  auto s16 = [](const uint8_t* p) { return int16_t(p[0] | p[1] << 8); };
  for (size_t pos = 0; pos + 7 * ch <= n; pos += align) {
    const uint8_t* b = d + pos;
    size_t len = std::min<size_t>(align, n - pos);
    int pred[2], delta[2], s1[2], s2[2];
    for (int c = 0; c < ch; c++) {
      pred[c] = b[c];
      if (pred[c] >= ncoef) return false;
      delta[c] = s16(b + ch + 2 * c);
      s1[c] = s16(b + 3 * ch + 2 * c);
      s2[c] = s16(b + 5 * ch + 2 * c);
    }
    for (int c = 0; c < ch; c++) out.push_back(int16_t(s2[c]));
    for (int c = 0; c < ch; c++) out.push_back(int16_t(s1[c]));
    int c = 0;
    for (size_t i = 7 * ch; i < len; i++) {
      for (int half = 0; half < 2; half++) {
        int nib = half == 0 ? b[i] >> 4 : b[i] & 15;
        int snib = nib >= 8 ? nib - 16 : nib;
        int predicted = (s1[c] * coef[2 * pred[c]] + s2[c] * coef[2 * pred[c] + 1]) / 256;
        int pcm = std::clamp(predicted + snib * delta[c], -32768, 32767);
        s2[c] = s1[c];
        s1[c] = pcm;
        delta[c] = std::max(16, (adapt[nib] * delta[c]) / 256);
        out.push_back(int16_t(pcm));
        c = (c + 1) % ch;
      }
    }
  }
  return true;
}

// Runs ffmpeg with the given arguments and waits; returns its exit code (-1: could not start).
static int run_ffmpeg(const std::vector<std::string>& args) {
#if defined(_WIN32)
  std::wstring cmd = L"\"" + fs::path(g_ffmpeg).wstring() + L"\"";
  for (auto& a : args) cmd += L" \"" + fs::path(a).wstring() + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return -1;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return int(code);
#else
  std::string cmd = "'" + g_ffmpeg + "'";
  for (auto& a : args) cmd += " '" + a + "'";
  return std::system(cmd.c_str());
#endif
}

// The 360 voices are 48000 Hz Ogg streams with serial number 0 and pages of about half a second
// (the game follows a voice's playback by them; lip-sync stayed still with Steam's 48011 Hz, one
// big page and a random serial). The output is made the same way: 48011 Hz is resampled to 48000.
static bool encode_ogg(const std::vector<int16_t>& pcm, int ch, int& sr, Bytes& ogg, std::string& why) {
  if (g_ffmpeg.empty()) { why = "MS ADPCM voice (needs ffmpeg)"; return false; }
  unsigned id = g_tmp_counter++;
  fs::path wav = g_tmp / ("v" + std::to_string(id) + ".wav"), oggp = g_tmp / ("v" + std::to_string(id) + ".ogg");
  {
    Bytes w(44);
    uint32_t data = uint32_t(pcm.size() * 2);
    memcpy(&w[0], "RIFF", 4); wr32(&w[4], 36 + data, false); memcpy(&w[8], "WAVEfmt ", 8);
    wr32(&w[16], 16, false); wr16(&w[20], 1, false); wr16(&w[22], uint16_t(ch), false); wr32(&w[24], sr, false);
    wr32(&w[28], sr * ch * 2, false); wr16(&w[32], uint16_t(ch * 2), false); wr16(&w[34], 16, false);
    memcpy(&w[36], "data", 4); wr32(&w[40], data, false);
    w.resize(44 + data);
    for (size_t i = 0; i < pcm.size(); i++) wr16(&w[44 + 2 * i], uint16_t(pcm[i]), false);
    write_file(wav, w);
  }
  const int out_sr = (sr > 47500 && sr < 48500) ? 48000 : sr;
  int rc = run_ffmpeg({"-v", "error", "-nostdin", "-y", "-i", wav.string(), "-ar", std::to_string(out_sr),
                       "-c:a", "libvorbis", "-q:a", "4", "-fflags", "+bitexact", "-flags:a", "+bitexact",
                       "-page_duration", "500000", oggp.string()});
  std::error_code ec;
  fs::remove(wav, ec);
  if (rc != 0 || !fs::exists(oggp)) { why = "ffmpeg could not encode Ogg Vorbis (code " + std::to_string(rc) + ")"; fs::remove(oggp, ec); return false; }
  ogg = read_file(oggp);
  fs::remove(oggp, ec);
  sr = out_sr;
  return true;
}

// Ogg page CRC (polynomial 0x04C11DB7, no reflection, as libogg).
static uint32_t ogg_crc(const uint8_t* d, size_t n) {
  static uint32_t table[256];
  static bool made = false;
  if (!made) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t r = i << 24;
      for (int k = 0; k < 8; k++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
      table[i] = r;
    }
    made = true;
  }
  uint32_t crc = 0;
  for (size_t i = 0; i < n; i++) crc = (crc << 8) ^ table[((crc >> 24) ^ d[i]) & 0xFF];
  return crc;
}

// Serial number 0 on every page, as in the 360 files (CRCs made again).
static void ogg_zero_serial(Bytes& ogg) {
  for (size_t q = 0; q + 27 <= ogg.size() && !memcmp(&ogg[q], "OggS", 4);) {
    int nseg = ogg[q + 26];
    if (q + 27 + nseg > ogg.size()) return;
    size_t body = 0;
    for (int k = 0; k < nseg; k++) body += ogg[q + 27 + k];
    size_t size = 27 + nseg + body;
    if (q + size > ogg.size()) return;
    memset(&ogg[q + 14], 0, 4);
    memset(&ogg[q + 22], 0, 4);
    uint32_t c = ogg_crc(&ogg[q], size);
    wr32(&ogg[q + 22], c, false);
    q += size;
  }
}

// Builds a big-endian 360 Ogg stream block (stream header + Ogg header + seek table + data).
struct OggLoop {
  uint32_t start_byte = 0, end_byte = 0;  // stream header loop fields (bytes of Ogg data)
  uint32_t mini4 = 0, mini8 = 0, mini18 = 0;  // Ogg header fields that go with them
  uint8_t xorb = 0x1E;                         // header XOR byte (0x1E: what most 360 voices use)
};
static bool ogg_block(const Bytes& ogg, int ch, int sr, Bytes& blk, std::string& why, const OggLoop& loop = OggLoop()) {
  struct Page { size_t off, size; int64_t gran; };
  std::vector<Page> pages;
  int packets = 0;
  size_t header_end = 0;  // end of the page that completes the third (setup) header packet
  for (size_t q = 0; q + 27 <= ogg.size();) {
    if (memcmp(&ogg[q], "OggS", 4)) { why = "encoder output is not Ogg"; return false; }
    int nseg = ogg[q + 26];
    if (q + 27 + nseg > ogg.size()) { why = "truncated Ogg page"; return false; }
    size_t body = 0;
    for (int k = 0; k < nseg; k++) {
      body += ogg[q + 27 + k];
      if (ogg[q + 27 + k] < 255 && packets < 3) packets++;
    }
    size_t size = 27 + nseg + body;
    if (q + size > ogg.size()) { why = "truncated Ogg page"; return false; }
    int64_t gran = 0;
    for (int k = 7; k >= 0; k--) gran = gran << 8 | ogg[q + 6 + k];
    pages.push_back({q, size, gran});
    q += size;
    if (packets == 3 && !header_end) header_end = q;
  }
  if (!header_end) { why = "no Vorbis headers"; return false; }
  size_t vorb = header_end;
  Bytes data(ogg.begin() + vorb, ogg.end());
  int64_t total = pages.empty() ? 0 : pages.back().gran;
  uint32_t step = uint32_t(sr / 10);
  std::vector<uint32_t> seek;
  for (int64_t smp = 0; smp < total; smp += step) {
    uint32_t off = 0;
    for (auto& p : pages)
      if (p.off >= vorb) {
        off = uint32_t(p.off - vorb);
        if (p.gran >= smp) break;
      }
    seek.push_back(off);
  }
  uint32_t extradata = uint32_t(0x20 + seek.size() * 4 + vorb);
  blk.assign(0x20 + extradata, 0);
  uint8_t* h = blk.data();
  wr32(h + 0x00, uint32_t(data.size()), true);
  wr32(h + 0x04, ch, true);
  wr32(h + 0x08, sr, true);
  wr32(h + 0x0C, 6, true);
  wr32(h + 0x10, loop.start_byte, true);
  wr32(h + 0x14, loop.end_byte, true);
  wr32(h + 0x18, extradata, true);
  uint8_t* m = h + 0x20;
  const uint8_t x = loop.xorb ? loop.xorb : 0x1E;
  m[0] = 2; m[1] = 0x20; m[2] = x;
  wr32(m + 0x04, loop.mini4, true);
  wr32(m + 0x08, loop.mini8, true);
  wr32(m + 0x0C, 0x3DCCCCCD, true);  // 0.1 s per seek entry
  wr32(m + 0x18, loop.mini18, true);
  wr32(m + 0x10, uint32_t(seek.size() * 4), true);
  wr32(m + 0x14, uint32_t(vorb), true);
  for (size_t i = 0; i < seek.size(); i++) wr32(m + 0x20 + 4 * i, seek[i], true);
  uint8_t* v = m + 0x20 + seek.size() * 4;
  for (size_t i = 0; i < vorb; i++) v[i] = ogg[i] ^ x;
  blk.insert(blk.end(), data.begin(), data.end());
  OggInfo check;
  if (!check_stream(blk.data(), blk.size(), true, check, why)) { why = "built Ogg block: " + why; return false; }
  return true;
}

// Steam MS ADPCM stream block (little-endian) -> 360 Ogg block.
static bool msadpcm_block_to_ogg(const uint8_t* b, size_t len, Bytes& out, std::string& why) {
  uint32_t ss = rd32(b, false), ch = rd32(b + 4, false), sr = rd32(b + 8, false);
  uint32_t ex = rd32(b + 0x18, false), aux = rd32(b + 0x1C, false);
  size_t p = 0x20;
  if (aux) {
    if (aux != 1 || memcmp(b + p, "MARK", 4)) { why = "unknown aux chunk"; return false; }
    p += rd32(b + p + 4, false);
  }
  if (0x20ull + ex + ss > len || p + 22 > 0x20ull + ex) { why = "MS ADPCM stream past the end"; return false; }
  const uint8_t* f = b + p;  // WAVEFORMATEX
  int align = rd16(f + 12, false), ncoef = rd16(f + 20, false);
  if (ncoef < 1 || ncoef > 32 || p + 22 + 4ull * ncoef > 0x20ull + ex) { why = "MS ADPCM format block"; return false; }
  std::vector<int16_t> coef(2 * ncoef);
  for (int i = 0; i < 2 * ncoef; i++) coef[i] = int16_t(rd16(f + 22 + 2 * i, false));
  std::vector<int16_t> pcm;
  if (!decode_msadpcm(b + 0x20 + ex, ss, int(ch), align, coef.data(), ncoef, pcm)) { why = "MS ADPCM decode"; return false; }
  Bytes ogg;
  int rate = int(sr);
  if (!encode_ogg(pcm, int(ch), rate, ogg, why)) return false;
  ogg_zero_serial(ogg);
  return ogg_block(ogg, int(ch), rate, out, why);
}

// Steam Ogg stream block (little-endian) -> 360 Ogg block built the same way as the
// re-encoded ones (plain Ogg pages extracted first, then wrapped again).
static bool ogg_block_rewrap(const uint8_t* b, size_t len, Bytes& out, std::string& why) {
  OggInfo o;
  if (!check_stream(b, len, false, o, why)) return false;
  if (o.codec == 0xFFFFFFFF) { why = "dummy stream"; return false; }
  size_t vstart = o.mini + 0x20 + o.seek;
  Bytes ogg(b + vstart, b + vstart + o.vorb + o.stream_size);
  if (o.version == 3) {
    uint8_t key = uint8_t(o.stream_size & 0xFF), b1 = key & 0x7F, b2 = key & 0x3F;
    for (size_t i = 0; i < ogg.size(); i++) ogg[i] ^= kOggV3Table[(b2 + i) & 0xFF] ^ b1;
  } else {
    for (size_t i = 0; i < o.vorb; i++) ogg[i] ^= o.xorb;
  }
  if (ogg.size() < 4 || memcmp(ogg.data(), "OggS", 4)) { why = "Ogg data did not decode"; return false; }
  ogg_zero_serial(ogg);
  OggLoop loop;
  loop.start_byte = rd32(b + 0x10, false);
  loop.end_byte = rd32(b + 0x14, false);
  loop.mini4 = rd32(b + o.mini + 4, false);
  loop.mini8 = rd32(b + o.mini + 8, false);
  loop.mini18 = rd32(b + o.mini + 0x18, false);
  if (o.version == 2) loop.xorb = o.xorb;
  return ogg_block(ogg, int(rd32(b + 4, false)), int(rd32(b + 8, false)), out, why, loop);
}

// us: 360 English file (big-endian); jp: Steam Japanese file (little-endian, or big-endian)
static bool srd_to_x360(Bytes& b);
static std::atomic<unsigned> g_voice_lip{0};  // converted voices that carry their lip-sync block

// Lip-sync of a voice line, after the SCD data (size at +0x10, 64-bit): an SRD block and a
// 16-byte footer "add srd", 0, u32 SRD size. Every Steam Japanese voice file has one; the 360
// field lines have one in big-endian (the game takes the lip-sync of a line from it, by name).
static bool scd_lip_block(const Bytes& d, bool be, size_t& body, Bytes& srd) {
  srd.clear();
  uint64_t size = be ? (uint64_t(rd32(&d[0x10], true)) << 32 | rd32(&d[0x14], true))
                     : (uint64_t(rd32(&d[0x14], false)) << 32 | rd32(&d[0x10], false));
  body = d.size();
  if (size < 0x50 || size >= d.size()) return false;
  if (d.size() - size < 32 || memcmp(&d[size], "SRD", 3) || memcmp(&d[d.size() - 16], "add srd", 8)) return false;
  uint32_t n = rd32(&d[size + 4], be);
  if (n < 18 || size + n + 16 != d.size() || rd32(&d[d.size() - 4], be) != n) return false;
  body = size_t(size);
  srd.assign(d.begin() + size, d.begin() + size + n);
  return true;
}

static bool convert_scd(const Bytes& us, const Bytes& jp, Bytes& out, std::string& why) {
  Scd u, j;
  if (!parse_scd(us, u, why)) { why = "360 file: " + why; return false; }
  if (!parse_scd(jp, j, why)) { why = "Steam file: " + why; return false; }
  if (!u.be) { why = "360 file is not big-endian"; return false; }
  if (u.metas.size() != j.metas.size()) {
    why = "stream count " + std::to_string(u.metas.size()) + " vs " + std::to_string(j.metas.size());
    return false;
  }
  if (u.metas.empty()) { out = us; return true; }  // empty bank
  uint32_t u0 = *std::min_element(u.metas.begin(), u.metas.end());
  for (size_t i = 0; i < 6; i++) {
    uint32_t t = rd32(&us[u.tables + 8 + 4 * i], true);
    if (i != 3 && i != 5 && t > u0) { why = "360 table after stream data"; return false; }
  }
  for (size_t i = 1; i < j.metas.size(); i++)
    if (j.metas[i] <= j.metas[i - 1]) { why = "Steam streams out of order"; return false; }
  size_t jbody;
  Bytes srd;
  const bool lip = scd_lip_block(jp, j.be, jbody, srd);
  if (lip && !j.be && !srd_to_x360(srd)) { why = "Steam lip-sync block"; return false; }
  out.assign(us.begin(), us.begin() + u0);
  for (size_t i = 0; i < j.metas.size(); i++) {
    size_t start = j.metas[i];
    size_t end = i + 1 < j.metas.size() ? j.metas[i + 1] : jbody;
    Bytes blk(jp.begin() + start, jp.begin() + end);
    uint32_t codec = rd32(&blk[0x0C], j.be);
    if (!j.be && codec == 12) {
      Bytes conv;
      if (!msadpcm_block_to_ogg(blk.data(), blk.size(), conv, why)) { why = "Steam stream: " + why; return false; }
      blk.swap(conv);
    } else if (!j.be && codec == 6) {
      Bytes conv;
      if (!ogg_block_rewrap(blk.data(), blk.size(), conv, why)) { why = "Steam stream: " + why; return false; }
      blk.swap(conv);
    } else if (j.be) {
      OggInfo o;
      if (!check_stream(blk.data(), blk.size(), true, o, why)) { why = "Steam stream: " + why; return false; }
    } else if (!block_to_be(blk.data(), blk.size(), why)) {
      why = "Steam stream: " + why;
      return false;
    }
    out.resize(align_up(out.size(), 16), 0);
    wr32(&out[u.headers_offset + 4 * i], uint32_t(out.size()), true);
    out.insert(out.end(), blk.begin(), blk.end());
  }
  out.resize(align_up(out.size(), 128), 0);  // the 360 files are padded to 128 bytes
  wr32(&out[0x10], 0, true);  // 64-bit file size
  wr32(&out[0x14], uint32_t(out.size()), true);
  if (lip) {
    g_voice_lip++;
    out.insert(out.end(), srd.begin(), srd.end());
    static const char tag[8] = {'a', 'd', 'd', ' ', 's', 'r', 'd', 0};
    out.insert(out.end(), tag, tag + 8);
    out.resize(out.size() + 8, 0);
    wr32(&out[out.size() - 4], uint32_t(srd.size()), true);
  }
  return true;
}

// ---------------------------------------------------------------- WPD

struct WpdEntry {
  std::string name, ext;
  uint32_t off = 0, size = 0;
};
static bool parse_wpd(const Bytes& d, bool& be, std::vector<WpdEntry>& ents) {
  if (d.size() < 16 || memcmp(d.data(), "WPD", 3)) return false;
  for (int pass = 0; pass < 2; pass++) {
    be = pass == 0;
    uint32_t n = rd32(&d[4], be);
    if (16ull + 32ull * n > d.size()) continue;
    ents.clear();
    bool ok = true;
    for (uint32_t i = 0; i < n && ok; i++) {
      const uint8_t* e = &d[16 + 32 * i];
      WpdEntry w;
      w.name.assign((const char*)e, strnlen((const char*)e, 16));
      w.off = rd32(e + 16, be);
      w.size = rd32(e + 20, be);
      w.ext.assign((const char*)e + 24, strnlen((const char*)e + 24, 8));
      if (uint64_t(w.off) + w.size > d.size()) ok = false;
      ents.push_back(w);
    }
    if (ok) return true;
  }
  return false;
}

struct WpdStats {
  int members = 0, converted = 0, kept = 0;
  std::string first_problem;
};
static bool convert_wpd(const Bytes& us, const Bytes& jp, Bytes& out, std::string& why, WpdStats& st) {
  bool ube, jbe;
  std::vector<WpdEntry> ue, je;
  if (!parse_wpd(us, ube, ue) || !ube) { why = "360 file is not a big-endian WPD"; return false; }
  if (!parse_wpd(jp, jbe, je)) { why = "Steam file is not a WPD"; return false; }
  std::map<std::string, const WpdEntry*> jmap;
  for (auto& e : je) jmap[e.name + "." + e.ext] = &e;
  size_t align = 2048;
  for (auto& e : ue)
    while (align > 16 && e.off % align) align >>= 1;
  out.assign(us.begin(), us.begin() + 16 + 32 * ue.size());
  size_t pos = ue.empty() ? out.size() : ue[0].off;
  for (size_t i = 0; i < ue.size(); i++) {
    const WpdEntry& e = ue[i];
    Bytes member(us.begin() + e.off, us.begin() + e.off + e.size);
    st.members++;
    auto it = jmap.find(e.name + "." + e.ext);
    if (it != jmap.end() && e.ext == "scd") {
      Bytes jm(jp.begin() + it->second->off, jp.begin() + it->second->off + it->second->size), conv;
      std::string w;
      if (convert_scd(member, jm, conv, w)) {
        member.swap(conv);
        st.converted++;
      } else {
        st.kept++;
        if (st.first_problem.empty()) st.first_problem = e.name + ": " + w;
      }
    } else {
      st.kept++;
    }
    pos = std::max(pos, align_up(out.size(), align));
    out.resize(pos, 0);
    wr32(&out[16 + 32 * i + 16], uint32_t(pos), true);
    wr32(&out[16 + 32 * i + 20], uint32_t(member.size()), true);
    out.insert(out.end(), member.begin(), member.end());
    pos = out.size();
  }
  out.resize(align_up(out.size(), align), 0);
  if (!st.converted) { why = "no member converted (" + st.first_problem + ")"; return false; }
  return true;
}

// ---------------------------------------------------------------- lip-sync and facial motion
// Steam keeps a Japanese copy of the event and character motion files (set "v"), the 360
// release only the English one. Both are WPD bundles; Japanese and English differ in their
// motion members (mtb: facial and lip motion of cutscenes) and in lsdpack's srd members (lip
// shapes for battle and field lines). mtb is the same on both platforms; srd differs only in
// byte order. Built here: the members of the Japanese file, each one taken from the 360 file
// where Japanese = Steam English, else converted from Steam. A member type is converted only
// if every member the 360 and the Steam English file share converts to the 360 one.

// Steam srd -> 360: "SRD", version 0/1, u32 size at +4; records from +18 {len, n, n byte
// pairs, u16 time}, ended by a record of length 0. The last record's time keeps its byte order.
static bool srd_to_x360(Bytes& b) {
  if (b.size() < 18 || memcmp(b.data(), "SRD", 3) || b[3] > 1) return false;
  swap32(&b[4]);
  std::vector<size_t> recs;
  size_t i = 18;
  while (i < b.size() && b[i]) {
    if (b[i] < 4 || i + b[i] > b.size()) return false;
    recs.push_back(i);
    i += b[i];
  }
  for (size_t k = 0; k + 1 < recs.size(); k++) std::swap(b[recs[k] + b[recs[k]] - 2], b[recs[k] + b[recs[k]] - 1]);
  return true;
}
static bool member_to_x360(const std::string& ext, Bytes& b) { return ext == "srd" ? srd_to_x360(b) : true; }

static bool merge_jp_motion(const Bytes& x, const Bytes& en, const Bytes& jp, Bytes& out, std::string& why,
                            int& taken) {
  bool xbe, ebe, jbe;
  std::vector<WpdEntry> xe, ee, je;
  if (!parse_wpd(x, xbe, xe) || !xbe) { why = "360 file is not a big-endian WPD"; return false; }
  if (!parse_wpd(en, ebe, ee) || !parse_wpd(jp, jbe, je)) { why = "Steam file is not a WPD"; return false; }
  auto slice = [](const Bytes& d, const WpdEntry& e) { return Bytes(d.begin() + e.off, d.begin() + e.off + e.size); };
  std::map<std::string, const WpdEntry*> xm, em;
  for (auto& e : xe) xm[e.name + "." + e.ext] = &e;
  for (auto& e : ee) em[e.name + "." + e.ext] = &e;
  std::map<std::string, int> good, bad;  // per member type: shared members that convert / do not
  for (auto& e : ee) {
    auto it = xm.find(e.name + "." + e.ext);
    if (it == xm.end()) continue;
    Bytes m = slice(en, e);
    (member_to_x360(e.ext, m) && m == slice(x, *it->second) ? good : bad)[e.ext]++;
  }
  std::vector<std::pair<const WpdEntry*, Bytes>> members;
  taken = 0;
  for (auto& e : je) {
    std::string key = e.name + "." + e.ext;
    Bytes m = slice(jp, e);
    auto ie = em.find(key);
    auto ix = xm.find(key);
    if (ie != em.end() && ix != xm.end() && m == slice(en, *ie->second)) {
      members.push_back({&e, slice(x, *ix->second)});
      continue;
    }
    bool known = e.ext == "mtb" || e.ext == "srd";
    if (bad[e.ext] || (!known && !good[e.ext])) {
      why = e.ext + " member " + e.name + " differs and its 360 form is not known";
      return false;
    }
    if (!member_to_x360(e.ext, m)) {
      why = e.ext + " member " + e.name + " could not be converted";
      return false;
    }
    members.push_back({&e, std::move(m)});
    taken++;
  }
  if (!taken) { why = "same as English"; return false; }
  size_t align = 2048;
  for (auto& e : xe)
    while (align > 16 && e.off % align) align >>= 1;
  out.assign(16 + 32 * members.size(), 0);
  memcpy(out.data(), x.data(), 4);
  wr32(&out[4], uint32_t(members.size()), true);
  for (size_t i = 0; i < members.size(); i++) {
    const WpdEntry& e = *members[i].first;
    uint8_t* d = &out[16 + 32 * i];
    memcpy(d, e.name.data(), std::min<size_t>(16, e.name.size()));
    memcpy(d + 24, e.ext.data(), std::min<size_t>(8, e.ext.size()));
    out.resize(align_up(out.size(), align), 0);
    d = &out[16 + 32 * i];
    wr32(d + 16, uint32_t(out.size()), true);
    wr32(d + 20, uint32_t(members[i].second.size()), true);
    out.insert(out.end(), members[i].second.begin(), members[i].second.end());
  }
  out.resize(align_up(out.size(), align), 0);
  return true;
}

// ---------------------------------------------------------------- patch writer

struct PatchFile {
  uint32_t code;
  uint8_t ftype;
  std::string path;
  uint64_t sector;
  uint32_t size;
};

static void write_filelist(const fs::path& p, std::vector<PatchFile> files) {
  std::sort(files.begin(), files.end(), [](const PatchFile& a, const PatchFile& b) {
    return std::tie(a.ftype, a.code) < std::tie(b.ftype, b.code);
  });
  std::vector<Bytes> chunks(1);
  struct E { uint32_t code; uint8_t ftype; unsigned chunk, off; };
  std::vector<E> ents;
  for (auto& f : files) {
    char text[1200];
    snprintf(text, sizeof(text), "%llx:%x:%x:%s", (unsigned long long)f.sector, f.size, f.size, f.path.c_str());
    size_t n = strlen(text) + 1;
    if (chunks.back().size() + n > 8192) chunks.emplace_back();
    ents.push_back({f.code, f.ftype, unsigned(chunks.size() - 1), unsigned(chunks.back().size())});
    chunks.back().insert(chunks.back().end(), text, text + n);
  }
  const char* end = "end";
  chunks.back().insert(chunks.back().end(), end, end + 4);
  if (chunks.size() > 512) throw std::runtime_error("too many files for one patch list");
  uint32_t count = uint32_t(ents.size());
  uint32_t chunk_tab = 12 + 8 * count, chunk_data = chunk_tab + 12 * uint32_t(chunks.size());
  Bytes out(chunk_data);
  wr32(&out[0], chunk_tab, false);
  wr32(&out[4], chunk_data, false);
  wr32(&out[8], count, false);
  for (uint32_t i = 0; i < count; i++) {
    uint8_t* e = &out[12 + 8 * i];
    wr32(e, ents[i].code, false);
    wr16(e + 4, uint16_t(ents[i].off | (ents[i].chunk & 1) << 15), false);
    e[6] = uint8_t(ents[i].chunk >> 1);
    e[7] = ents[i].ftype;
  }
  uint32_t pos = 0;
  for (size_t c = 0; c < chunks.size(); c++) {
    Bytes z = zlib_stored(chunks[c]);
    uint8_t* t = &out[chunk_tab + 12 * c];
    wr32(t, uint32_t(chunks[c].size()), false);
    wr32(t + 4, uint32_t(z.size()), false);
    wr32(t + 8, pos, false);
    pos += uint32_t(z.size());
    out.insert(out.end(), z.begin(), z.end());
  }
  write_file(p, out);
}

// ---------------------------------------------------------------- main build

static std::string lower(std::string s) {
  for (auto& c : s) c = char(tolower((unsigned char)c));
  return s;
}

// Loads sys/<list> and the zone lists it names. lang: "u" (360) or "v"/"a" (Steam).
static std::vector<Source> load_sources(const fs::path& weiss, const std::string& lang, const std::string& plat) {
  std::vector<Source> out;
  // filelist / filelist2 (white_img / white_img2) and filelist_scr (white_scr: event and zone texts)
  for (const char* n : {"", "2", "_scr"}) {
    Source s;
    s.name = std::string("filelist") + n + lang;
    fs::path list = weiss / "sys" / (s.name + "." + plat + ".bin");
    const bool scr = std::string(n) == "_scr";
    fs::path arc = weiss / "sys" / (std::string(scr ? "white_scr" : std::string("white_img") + n) + lang + "." + plat + ".bin");
    if (scr && (!fs::exists(list) || !fs::exists(arc))) continue;
    s.entries = parse_filelist(read_file(list));
    s.archive = std::make_shared<Archive>(arc);
    out.push_back(s);
  }
  std::vector<Source> zones;
  for (auto& s : out) {
    for (auto& e : s.entries) {
      size_t z = e.path.find("zone/filelist_z");
      if (z == std::string::npos || e.path.size() < 4 || e.path.substr(e.path.size() - 4) != ".bin") continue;
      std::string zid = e.path.substr(z + 14, 5);  // z0100
      fs::path arc = weiss / "zone" / ("white_" + zid + lang + "_img." + plat + ".bin");
      if (!fs::exists(arc)) {
        printf("  (no %s, skipping zone %s)\n", arc.filename().string().c_str(), zid.c_str());
        continue;
      }
      Source zs;
      zs.name = "zone " + zid;
      zs.entries = parse_filelist(s.archive->read(e));
      zs.archive = std::make_shared<Archive>(arc);
      zones.push_back(zs);
    }
  }
  out.insert(out.end(), zones.begin(), zones.end());
  return out;
}

static bool is_english_voice(const std::string& p) {
  if (p.compare(0, 11, "sound/pack/")) return false;
  if (p.find("/usa/") == std::string::npos) return false;
  size_t n = p.size();
  return n > 4 && (p.compare(n - 4, 4, ".scd") == 0 || p.compare(n - 4, 4, ".wpd") == 0);
}
static std::string jp_path_for(std::string p) {
  size_t u = p.find("/usa/");
  p.erase(u, 4);
  size_t x = p.find(".x360.");
  if (x != std::string::npos) p.replace(x, 6, ".win32.");
  return p;
}

static int build(const fs::path& root360, const fs::path& rootSteam, const fs::path& outDir, const fs::path& samples) {
  fs::path w360 = root360 / "weiss_data", wSteam = rootSteam / "weiss_data";
  printf("Reading the 360 file lists\n");
  auto us = load_sources(w360, "u", "x360");
  printf("Reading the Steam (Japanese) file lists\n");
  auto jp = load_sources(wSteam, "v", "win32");
  for (auto* set : {&us, &jp})
    for (auto& s : *set) printf("  %-14s %6zu files  %s\n", s.name.c_str(), s.entries.size(), s.archive->path.filename().string().c_str());

  struct Ref { Source* src; const Entry* e; };
  std::map<std::pair<uint8_t, uint32_t>, Ref> jpByCode;
  std::map<std::string, Ref> jpByPath;
  for (auto& s : jp)
    for (auto& e : s.entries) {
      jpByCode.emplace(std::make_pair(e.ftype, e.code), Ref{&s, &e});
      jpByPath.emplace(lower(e.path), Ref{&s, &e});
    }
  std::vector<std::pair<Ref, Ref>> jobs;
  std::set<std::pair<uint8_t, uint32_t>> seen;
  int noJp = 0, pathMismatch = 0;
  fs::create_directories(outDir);
  FILE* report = fopen((outDir / "report.csv").string().c_str(), "w");
  if (!report) throw std::runtime_error("cannot write report.csv");
  fprintf(report, "path,result,detail,size_360,size_steam,size_out\n");
  for (auto& s : us)
    for (auto& e : s.entries) {
      if (!is_english_voice(e.path) || !seen.insert({e.ftype, e.code}).second) continue;
      std::string want = lower(jp_path_for(e.path));
      auto it = jpByCode.find({e.ftype, e.code});
      Ref j{nullptr, nullptr};
      if (it != jpByCode.end() && lower(it->second.e->path) == want) {
        j = it->second;
      } else {
        auto ip = jpByPath.find(want);
        if (ip != jpByPath.end()) { j = ip->second; pathMismatch++; }
      }
      if (!j.e) {
        noJp++;
        fprintf(report, "%s,kept English,no Japanese file,%u,,\n", e.path.c_str(), e.usize);
        continue;
      }
      jobs.push_back({Ref{&s, &e}, j});
    }
  printf("English voice files: %zu with a Japanese version, %d without (those stay English)\n", jobs.size() + 0, noJp);
  if (pathMismatch) printf("  (%d matched by name instead of file code)\n", pathMismatch);

  fs::path arcPath = outDir / "white_patch_u.x360.bin.tmp";
  std::ofstream arc(arcPath, std::ios::binary);
  if (!arc) throw std::runtime_error("cannot write " + arcPath.string());
  std::vector<PatchFile> files;
  uint64_t written = 0;
  int ok = 0, failed = 0, samplesScd = 0, samplesWpd = 0, samplesBad = 0, done = 0;
  std::map<std::string, int> reasons;
  if (!samples.empty()) fs::create_directories(samples);
  auto save_sample = [&](const char* kind, const std::string& path, const Bytes& a, const Bytes& b, const Bytes* c) {
    std::string base = path.substr(path.rfind('/') + 1);
    base = base.substr(0, base.find('.'));
    write_file(samples / (std::string(kind) + "_" + base + ".360"), a);
    write_file(samples / (std::string(kind) + "_" + base + ".steam"), b);
    if (c) write_file(samples / (std::string(kind) + "_" + base + ".out"), *c);
  };
  // Archives are read on this thread; conversions (MS ADPCM -> Ogg runs ffmpeg) run on a
  // pool of worker threads; results are written here in whatever order they finish.
  struct Task {
    size_t job = 0;
    Bytes ub, jb, ob;
    std::string why;
    bool good = false, wpd = false;
    WpdStats st;
  };
  std::mutex mu;
  std::condition_variable cv_in, cv_out;
  std::deque<std::unique_ptr<Task>> inq, outq;
  bool closing = false;
  unsigned nworkers = std::max(2u, std::min(16u, std::thread::hardware_concurrency()));
  std::vector<std::thread> workers;
  for (unsigned w = 0; w < nworkers; w++)
    workers.emplace_back([&] {
      for (;;) {
        std::unique_ptr<Task> t;
        {
          std::unique_lock<std::mutex> lk(mu);
          cv_in.wait(lk, [&] { return closing || !inq.empty(); });
          if (inq.empty()) return;
          t = std::move(inq.front());
          inq.pop_front();
        }
        if (t->why.empty()) {
          try {
            t->good = t->wpd ? convert_wpd(t->ub, t->jb, t->ob, t->why, t->st) : convert_scd(t->ub, t->jb, t->ob, t->why);
          } catch (const std::exception& ex) {
            t->why = ex.what();
          }
        }
        std::lock_guard<std::mutex> lk(mu);
        outq.push_back(std::move(t));
        cv_out.notify_one();
      }
    });
  auto finish = [&](Task& t) {
    const Entry& ue = *jobs[t.job].first.e;
    const Entry& je = *jobs[t.job].second.e;
    const Bytes& ub = t.ub;
    const Bytes& jb = t.jb;
    const Bytes& ob = t.ob;
    bool wpd = t.wpd;
    std::string why = t.why;
    WpdStats& st = t.st;
    if (t.good) {
      ok++;
      PatchFile pf{ue.code, ue.ftype, ue.path, written / 2048, uint32_t(ob.size())};
      arc.write((const char*)ob.data(), ob.size());
      written += ob.size();
      size_t pad = align_up(written, 2048) - written;
      static const char zeros[2048] = {0};
      arc.write(zeros, pad);
      written += pad;
      files.push_back(pf);
      std::string detail = wpd ? (std::to_string(st.converted) + " of " + std::to_string(st.members) + " sounds") : "";
      if (wpd && st.kept && !st.first_problem.empty()) detail += " (" + st.first_problem + ")";
      for (auto& ch : detail) if (ch == ',') ch = ';';
      fprintf(report, "%s,Japanese,%s,%u,%u,%zu\n", ue.path.c_str(), detail.c_str(), ue.usize, je.usize, ob.size());
      if (!samples.empty() && ((!wpd && samplesScd < 3) || (wpd && samplesWpd < 1))) {
        save_sample(wpd ? "wpd" : "scd", ue.path, ub, jb, &ob);
        (wpd ? samplesWpd : samplesScd)++;
      }
    } else {
      failed++;
      reasons[why.size() > 60 ? why.substr(0, 60) : why]++;
      for (auto& ch : why) if (ch == ',') ch = ';';
      fprintf(report, "%s,kept English,%s,%u,%u,\n", ue.path.c_str(), why.c_str(), ue.usize, je.usize);
      if (!samples.empty() && samplesBad < 4 && !ub.empty() && !jb.empty() && ub.size() + jb.size() < 8u << 20) {
        save_sample("failed", ue.path, ub, jb, nullptr);
        samplesBad++;
      }
    }
    if (++done % 1000 == 0) printf("  %d / %zu (%.0f MB written)\n", done, jobs.size(), written / 1048576.0);
    fflush(stdout);
  };
  size_t submitted = 0;
  for (size_t i = 0; i < jobs.size(); i++) {
    auto t = std::make_unique<Task>();
    t->job = i;
    const Entry& ue = *jobs[i].first.e;
    t->wpd = ue.path.compare(ue.path.size() - 4, 4, ".wpd") == 0;
    try {
      t->ub = jobs[i].first.src->archive->read(ue);
      t->jb = jobs[i].second.src->archive->read(*jobs[i].second.e);
    } catch (const std::exception& ex) {
      t->why = ex.what();
    }
    {
      std::lock_guard<std::mutex> lk(mu);
      inq.push_back(std::move(t));
      cv_in.notify_one();
    }
    submitted++;
    // keep at most 4 tasks per worker in flight; write finished ones meanwhile
    for (;;) {
      std::unique_ptr<Task> r;
      {
        std::unique_lock<std::mutex> lk(mu);
        if (submitted - size_t(done) <= 4 * nworkers && outq.empty()) break;
        cv_out.wait(lk, [&] { return !outq.empty(); });
        r = std::move(outq.front());
        outq.pop_front();
      }
      finish(*r);
    }
  }
  while (size_t(done) < submitted) {
    std::unique_ptr<Task> r;
    {
      std::unique_lock<std::mutex> lk(mu);
      cv_out.wait(lk, [&] { return !outq.empty(); });
      r = std::move(outq.front());
      outq.pop_front();
    }
    finish(*r);
  }
  {
    std::lock_guard<std::mutex> lk(mu);
    closing = true;
  }
  cv_in.notify_all();
  for (auto& w : workers) w.join();

  // Japanese lip-sync and facial motion (event DataSets, character motion and lsdpack files).
  int lipOk = 0, lipSame = 0, lipKept = 0, textOk = 0, textMissing = 0, textBad = 0;
  std::map<std::string, int> lipReasons;
  try {
    printf("Reading the Steam (English) file lists for the lip-sync comparison\n");
    auto en = load_sources(wSteam, "a", "win32");
    std::map<std::string, Ref> enByPath;
    std::map<std::string, std::vector<Ref>> usByPath;
    for (auto& s : en)
      for (auto& e : s.entries) enByPath.emplace(lower(e.path), Ref{&s, &e});
    for (auto& s : us)
      for (auto& e : s.entries) usByPath[lower(e.path)].push_back(Ref{&s, &e});
    std::set<std::string> done_paths;
    // jp_lipsync.txt: "<key> <path>" of the lip-sync / motion / voice table files in the patch
    // (the game logs the first lookups of them: LR_JP_LIPSYNC_LIST).
    FILE* lipKeys = fopen((outDir / "jp_lipsync.txt").string().c_str(), "w");
    struct LipKeysClose { FILE*& f; ~LipKeysClose() { if (f) fclose(f); } } lipKeysClose{lipKeys};
    for (auto& s : jp)
      for (auto& e : s.entries) {
        std::string p = lower(e.path);
        const bool db = p.compare(0, 12, "db/resident/") == 0;
        if (p.compare(0, 6, "event/") && p.compare(0, 4, "mot/") && !db) continue;
        size_t n = p.size();
        if (n < 4 || (p.compare(n - 4, 4, ".bin") && p.compare(n - 4, 4, ".wpd"))) continue;
        if (!done_paths.insert(p).second) continue;
        // English name: same, or with _us before the first dot of the file name (a00_us.white...)
        auto ie = enByPath.find(p);
        if (ie == enByPath.end()) {
          std::string q = p;
          size_t slash = q.rfind('/'), dot = q.find('.', slash == std::string::npos ? 0 : slash);
          if (dot != std::string::npos) q.insert(dot, "_us");
          ie = enByPath.find(q);
        }
        if (ie == enByPath.end()) continue;
        std::string xp = lower(ie->second.e->path);
        size_t w = xp.find(".win32.");
        if (w == std::string::npos) continue;
        xp.replace(w, 7, ".x360.");
        auto ix = usByPath.find(xp);
        if (ix == usByPath.end()) continue;
        Bytes jb, eb, xb, ob;
        std::string why;
        int taken = 0;
        try {
          jb = s.archive->read(e);
          eb = ie->second.src->archive->read(*ie->second.e);
          if (jb == eb) { lipSame++; continue; }
          xb = ix->second[0].src->archive->read(*ix->second[0].e);
          if (db) {
            // Voice data tables (sound_filename_dic: which lip-sync entry each line uses): the
            // Japanese one as it is, when the 360 file is byte for byte the Steam English one.
            if (xb != eb) throw std::runtime_error("360 table differs from the Steam English one");
            ob = jb;
            taken = 1;
          } else if (!merge_jp_motion(xb, eb, jb, ob, why, taken)) {
            if (why == "same as English") { lipSame++; continue; }
            throw std::runtime_error(why);
          }
        } catch (const std::exception& ex) {
          lipKept++;
          std::string r = ex.what();
          lipReasons[r.size() > 60 ? r.substr(0, 60) : r]++;
          for (auto& ch : r) if (ch == ',') ch = ';';
          fprintf(report, "%s,kept English lip-sync,%s,%u,%u,\n", ix->second[0].e->path.c_str(), r.c_str(),
                  ix->second[0].e->usize, e.usize);
          continue;
        }
        std::set<std::pair<uint8_t, uint32_t>> codes;
        for (auto& r : ix->second) {
          if (!codes.insert({r.e->ftype, r.e->code}).second) continue;
          files.push_back(PatchFile{r.e->code, r.e->ftype, r.e->path, written / 2048, uint32_t(ob.size())});
          if (lipKeys) fprintf(lipKeys, "%llx %s\n", (unsigned long long)r.e->ftype << 32 | r.e->code, r.e->path.c_str());
        }
        arc.write((const char*)ob.data(), ob.size());
        written += ob.size();
        static const char zeros[2048] = {0};
        size_t pad = align_up(written, 2048) - written;
        arc.write(zeros, pad);
        written += pad;
        lipOk++;
        fprintf(report, "%s,Japanese lip-sync,%d member(s)%s,%u,%u,%zu\n", ix->second[0].e->path.c_str(), taken, db ? " (table)" : "",
                ix->second[0].e->usize, e.usize, ob.size());
        if (lipOk % 200 == 0) printf("  lip-sync: %d files\n", lipOk);
        fflush(stdout);
      }

    // Japanese on-screen text: the Steam release's txtres_jp.ztr next to every 360
    // txtres_us.ztr (same big-endian format; the code differs from the English one only in
    // the language digit, 1 -> 0). jp_text.txt lists "<English key> <Japanese key>" (key =
    // file type << 32 | code) for the game's Settings > Language.
    FILE* keys = fopen((outDir / "jp_text.txt").string().c_str(), "w");
    if (!keys) throw std::runtime_error("cannot write jp_text.txt");
    for (auto& [p, refs] : usByPath) {
      size_t n = p.size();
      if (p.compare(0, 7, "txtres/") || n < 13 || p.compare(n - 13, 13, "txtres_us.ztr")) continue;
      std::string jpp = p.substr(0, n - 13) + "txtres_jp.ztr";
      const Ref* jr = nullptr;
      if (auto a = jpByPath.find(jpp); a != jpByPath.end()) jr = &a->second;
      else if (auto b = enByPath.find(jpp); b != enByPath.end()) jr = &b->second;
      if (!jr) { textMissing++; continue; }
      const Entry& je = *jr->e;
      const Entry& ue = *refs[0].e;
      uint32_t diff = je.code ^ ue.code;
      int digits = 0;
      for (int d = 0; d < 8; d++) digits += ((diff >> (4 * d)) & 15) != 0;
      if (je.ftype != ue.ftype || digits != 1) {
        textBad++;
        fprintf(report, "%s,kept English text,code %u does not pair with %u,,,\n", je.path.c_str(), je.code, ue.code);
        continue;
      }
      Bytes tb;
      try {
        tb = jr->src->archive->read(je);
      } catch (const std::exception& ex) {
        textBad++;
        fprintf(report, "%s,kept English text,%s,,,\n", je.path.c_str(), ex.what());
        continue;
      }
      if (tb.size() < 8 || rd32(&tb[4], true) != 1) {
        textBad++;
        fprintf(report, "%s,kept English text,not a big-endian ZTR,,,\n", je.path.c_str());
        continue;
      }
      files.push_back(PatchFile{je.code, je.ftype, je.path, written / 2048, uint32_t(tb.size())});
      arc.write((const char*)tb.data(), tb.size());
      written += tb.size();
      static const char zeros[2048] = {0};
      size_t pad = align_up(written, 2048) - written;
      arc.write(zeros, pad);
      written += pad;
      std::set<std::pair<uint8_t, uint32_t>> codes;
      for (auto& r : refs)
        if (codes.insert({r.e->ftype, r.e->code}).second)
          fprintf(keys, "%llx %llx\n", (unsigned long long)r.e->ftype << 32 | r.e->code,
                  (unsigned long long)je.ftype << 32 | je.code);
      textOk++;
    }
    fclose(keys);
  } catch (const std::exception& ex) {
    printf("  lip-sync / text skipped: %s\n", ex.what());
  }
  printf("Japanese text: %d files, %d not in the Steam copy, %d kept English\n", textOk, textMissing, textBad);
  printf("Japanese lip-sync: %d files converted, %d already the same, %d kept English\n", lipOk, lipSame, lipKept);
  for (auto& r : lipReasons) printf("  lip-sync kept English: %d x %s\n", r.second, r.first.c_str());
  arc.close();
  fclose(report);
  printf("Converted %d, kept English %d\n", ok, failed);
  printf("Voice lip-sync: %u voice files carry their Japanese lip-sync block\n", g_voice_lip.load());
  for (auto& r : reasons) printf("  kept English: %d x %s\n", r.second, r.first.c_str());
  if (!ok && !lipOk && !textOk) {
    fs::remove(arcPath);
    return 2;
  }
  fs::path finalArc = outDir / "white_patch_u.x360.bin";
  if (fs::exists(finalArc)) fs::remove(finalArc);
  fs::rename(arcPath, finalArc);
  write_filelist(outDir / "filelist_patch_u.x360.bin", files);
  printf("Wrote %s (%.0f MB, %zu files)\n", finalArc.string().c_str(), written / 1048576.0, files.size());
  return ok * 2 >= ok + failed ? 0 : 3;
}

// --extract: copies the files whose path contains one of the patterns out of the 360 set (u)
// and the two Steam sets (a = English voice, v = Japanese voice), uncompressed, to
// <out>/<set>/<path>, for comparing the platforms' versions of a file. Read-only.
static int extract(const fs::path& root360, const fs::path& rootSteam, const fs::path& outDir,
                   const std::vector<std::string>& patterns) {
  struct Set {
    const char* name;
    fs::path weiss;
    const char* lang;
    const char* plat;
  };
  const Set sets[] = {{"360", root360 / "weiss_data", "u", "x360"},
                      {"steam_en", rootSteam / "weiss_data", "a", "win32"},
                      {"steam_jp", rootSteam / "weiss_data", "v", "win32"}};
  uint64_t total = 0;
  int written = 0;
  for (const Set& set : sets) {
    std::vector<Source> sources;
    try {
      sources = load_sources(set.weiss, set.lang, set.plat);
    } catch (const std::exception& ex) {
      printf("  %s: %s\n", set.name, ex.what());
      continue;
    }
    for (auto& src : sources) {
      for (auto& e : src.entries) {
        bool hit = false;
        for (auto& pat : patterns) hit |= e.path.find(pat) != std::string::npos;
        if (!hit) continue;
        if (total > (256ull << 20)) {
          printf("  (256 MB reached; %s skipped)\n", e.path.c_str());
          continue;
        }
        try {
          Bytes d = src.archive->read(e);
          fs::path out = outDir / set.name / fs::path(e.path).relative_path();
          fs::create_directories(out.parent_path());
          write_file(out, d);
          total += d.size();
          ++written;
          printf("  %s %s (%zu bytes)\n", set.name, e.path.c_str(), d.size());
        } catch (const std::exception& ex) {
          printf("  %s %s: %s\n", set.name, e.path.c_str(), ex.what());
        }
      }
    }
  }
  printf("%d files, %llu bytes\n", written, (unsigned long long)total);
  return written ? 0 : 2;
}

// --dlc: the Japanese voice lines of the downloadable content. Every installed DLC pack
// (<content>/0000000000000000/5351081B/00000002/<pack>/filelist_p<N>img_u.x360.bin: the 360
// packs and the ones converted from Steam) has its English lines under sound/pack/<n>/usa/;
// the Steam release keeps the Japanese ones, same file codes, in weiss_data/dlc/<N>/
// filelist_p<N>img_v (img_a: English). They are converted like the base game's and added to
// the existing patch in <patch>: appended after the base game's files (dlc_base.txt keeps
// where those end, so running again replaces the DLC files instead of adding them twice).
// Only the patch folder is written.
// Replaces game files with files of the same name (mods, e.g. a character model): each file is
// matched by its name to one entry of the game's own file lists (c392.x360.trb ->
// chr/pc/c392/bin/c392.x360.trb), appended to the patch archive and listed under that entry's
// code, so the game loads it instead of its own. The list as it was before the first one is kept
// as filelist_patch_u.x360.bin.before_mods; mods.txt names the files the patch replaces.
// remove: the named files are taken out of the list again (the game's own are used).
static int patch_files(const fs::path& patchDir, const fs::path& root360, const std::vector<fs::path>& files, bool remove) {
  const fs::path listPath = patchDir / "filelist_patch_u.x360.bin", arcPath = patchDir / "white_patch_u.x360.bin";
  if (!fs::exists(listPath) || !fs::exists(arcPath))
    throw std::runtime_error("no patch in " + patchDir.string() + " (the Japanese voice step makes it)");
  auto sources = load_sources(root360 / "weiss_data", "u", "x360");
  struct Target { const Entry* e; fs::path file; Bytes data; };
  std::vector<Target> targets;
  for (auto& f : files) {
    const std::string name = lower(f.filename().string());
    std::map<std::pair<uint8_t, uint32_t>, const Entry*> hits;
    for (auto& s : sources)
      for (auto& e : s.entries) {
        std::string p = lower(e.path);
        size_t slash = p.rfind('/');
        if ((slash == std::string::npos ? p : p.substr(slash + 1)) == name) hits.emplace(std::make_pair(e.ftype, e.code), &e);
      }
    if (hits.empty()) throw std::runtime_error(name + " is not the name of a file of the game");
    if (hits.size() > 1) {
      for (auto& h : hits) printf("  %s\n", h.second->path.c_str());
      throw std::runtime_error("more than one game file is called " + name);
    }
    Target t{hits.begin()->second, f, {}};
    if (!remove) {
      t.data = read_file(f);
      if (t.data.empty()) throw std::runtime_error(f.string() + " is empty");
    }
    targets.push_back(std::move(t));
  }
  const fs::path backup = patchDir / "filelist_patch_u.x360.bin.before_mods";
  if (!fs::exists(backup)) fs::copy_file(listPath, backup);
  std::set<std::pair<uint8_t, uint32_t>> codes;
  for (auto& t : targets) codes.insert({t.e->ftype, t.e->code});
  std::vector<PatchFile> list;
  for (auto& e : parse_filelist(read_file(listPath)))
    if (!codes.count({e.ftype, e.code})) list.push_back(PatchFile{e.code, e.ftype, e.path, e.sector, e.usize});
  if (!remove) {
    // the data first, then the list naming it
    std::fstream arc(arcPath, std::ios::binary | std::ios::in | std::ios::out | std::ios::ate);
    if (!arc) throw std::runtime_error("cannot write " + arcPath.string());
    uint64_t pos = uint64_t(arc.tellp());
    static const char zeros[2048] = {0};
    for (auto& t : targets) {
      uint64_t start = align_up(pos, 2048);
      arc.write(zeros, std::streamsize(start - pos));
      arc.write((const char*)t.data.data(), std::streamsize(t.data.size()));
      pos = start + t.data.size();
      list.push_back(PatchFile{t.e->code, t.e->ftype, t.e->path, start / 2048, uint32_t(t.data.size())});
      printf("  %s -> %s (%zu bytes; the game's own: %u)\n", t.file.filename().string().c_str(), t.e->path.c_str(),
             t.data.size(), t.e->usize);
    }
    arc.write(zeros, std::streamsize(align_up(pos, 2048) - pos));
    arc.close();
    if (!arc) throw std::runtime_error("writing " + arcPath.string() + " failed");
  }
  write_filelist(listPath, list);
  // mods.txt: the game files the patch replaces now
  std::map<std::string, std::string> mods;
  {
    std::ifstream in(patchDir / "mods.txt");
    std::string line;
    while (std::getline(in, line)) {
      size_t tab = line.find('\t');
      if (tab != std::string::npos) mods[line.substr(0, tab)] = line.substr(tab + 1);
    }
  }
  for (auto& t : targets) {
    if (remove) {
      printf("  %s: the game's own file again\n", t.e->path.c_str());
      mods.erase(t.e->path);
    } else {
      mods[t.e->path] = t.file.filename().string();
    }
  }
  std::ofstream out(patchDir / "mods.txt", std::ios::trunc);
  for (auto& m : mods) out << m.first << '\t' << m.second << '\n';
  printf("Patch now replaces %zu game file(s) with mods.\n", mods.size());
  return 0;
}

static int add_dlc(const fs::path& patchDir, const fs::path& rootSteam, const std::vector<fs::path>& contents) {
  const fs::path listPath = patchDir / "filelist_patch_u.x360.bin", arcPath = patchDir / "white_patch_u.x360.bin";
  if (!fs::exists(listPath) || !fs::exists(arcPath))
    throw std::runtime_error("no Japanese voice patch in " + patchDir.string() + " (run LR_JP_Voice.ps1 first)");
  // English DLC voices, first pack wins for a file code
  struct Pack { std::string num; fs::path dir; Source us, jp; };
  std::vector<Pack> packs;
  std::set<std::string> nums;
  for (auto& content : contents) {
    fs::path base = content / "0000000000000000" / "5351081B" / "00000002";
    if (!fs::is_directory(base)) { printf("  (no DLC folder %s)\n", base.string().c_str()); continue; }
    std::vector<fs::path> dirs;
    for (auto& d : fs::directory_iterator(base))
      if (d.is_directory()) dirs.push_back(d.path());
    std::sort(dirs.begin(), dirs.end());
    for (auto& d : dirs)
      for (auto& f : fs::directory_iterator(d)) {
        std::string n = f.path().filename().string();
        if (n.size() != 31 || n.compare(0, 10, "filelist_p") || n.compare(17, 14, "img_u.x360.bin")) continue;
        Pack p;
        p.num = n.substr(10, 7);
        p.dir = d;
        fs::path wh = d / ("white_p" + p.num + "img_u.x360.bin");
        if (!fs::exists(wh)) continue;
        fs::path sd = rootSteam / "weiss_data" / "dlc" / p.num;
        fs::path jl = sd / ("filelist_p" + p.num + "img_v.win32.bin"), jw = sd / ("white_p" + p.num + "img_v.win32.bin");
        if (!fs::exists(jl) || !fs::exists(jw)) {
          printf("  pack %s (%s): the Steam copy has no Japanese version (%s)\n", p.num.c_str(), d.filename().string().c_str(), jl.string().c_str());
          continue;
        }
        if (!nums.insert(p.num).second) {
          printf("  pack %s (%s): already read from another folder\n", p.num.c_str(), d.filename().string().c_str());
          continue;
        }
        try {  // a pack being installed right now: skipped, the next run takes it
          p.us.name = "dlc " + p.num;
          p.us.entries = parse_filelist(read_file(f.path()));
          p.us.archive = std::make_shared<Archive>(wh);
          p.jp.name = "steam dlc " + p.num;
          p.jp.entries = parse_filelist(read_file(jl));
          p.jp.archive = std::make_shared<Archive>(jw);
        } catch (const std::exception& ex) {
          printf("  pack %s (%s): skipped, could not read it (%s)\n", p.num.c_str(), d.filename().string().c_str(), ex.what());
          nums.erase(p.num);
          continue;
        }
        packs.push_back(std::move(p));
      }
  }
  printf("DLC packs with Japanese voices in the Steam copy: %zu\n", packs.size());
  struct Job { Pack* pack; const Entry* ue; const Entry* je; Bytes ob; std::string why; bool good = false; WpdStats st; };
  std::vector<Job> jobs;
  std::set<std::pair<uint8_t, uint32_t>> seen;
  auto stem = [](const std::string& p) {  // file name without platform and extension
    std::string n = lower(p.substr(p.rfind('/') + 1));
    return n.substr(0, n.find('.'));
  };
  for (auto& p : packs) {
    std::map<std::pair<uint8_t, uint32_t>, const Entry*> jpByCode;
    for (auto& e : p.jp.entries) jpByCode.emplace(std::make_pair(e.ftype, e.code), &e);
    int n = 0, missing = 0;
    for (auto& e : p.us.entries) {
      if (!is_english_voice(e.path) || !seen.insert({e.ftype, e.code}).second) continue;
      auto it = jpByCode.find({e.ftype, e.code});
      if (it == jpByCode.end() || stem(it->second->path) != stem(e.path) || it->second->path.find("/usa/") != std::string::npos) {
        missing++;
        continue;
      }
      jobs.push_back(Job{&p, &e, it->second});
      n++;
    }
    printf("  pack %s (%s): %d voice file(s)%s\n", p.num.c_str(), p.dir.filename().string().c_str(), n,
           missing ? (", " + std::to_string(missing) + " without a Japanese version").c_str() : "");
  }
  if (jobs.empty()) { printf("No DLC voice lines to convert.\n"); return 2; }
  // read on this thread, convert on several (ffmpeg)
  std::vector<Bytes> ub(jobs.size()), jb(jobs.size());
  for (size_t i = 0; i < jobs.size(); i++) {
    try {
      ub[i] = jobs[i].pack->us.archive->read(*jobs[i].ue);
      jb[i] = jobs[i].pack->jp.archive->read(*jobs[i].je);
    } catch (const std::exception& ex) {
      jobs[i].why = ex.what();
    }
  }
  std::atomic<size_t> next{0};
  std::vector<std::thread> workers;
  unsigned nworkers = std::max(2u, std::min(16u, std::thread::hardware_concurrency()));
  for (unsigned w = 0; w < nworkers; w++)
    workers.emplace_back([&] {
      for (size_t i; (i = next.fetch_add(1)) < jobs.size();) {
        Job& j = jobs[i];
        if (!j.why.empty()) continue;
        bool wpd = j.ue->path.size() > 4 && j.ue->path.compare(j.ue->path.size() - 4, 4, ".wpd") == 0;
        try {
          j.good = wpd ? convert_wpd(ub[i], jb[i], j.ob, j.why, j.st) : convert_scd(ub[i], jb[i], j.ob, j.why);
        } catch (const std::exception& ex) {
          j.why = ex.what();
        }
      }
    });
  for (auto& t : workers) t.join();

  // the patch: base game files stay where they are; earlier DLC files (past dlc_base.txt) go
  const fs::path baseTxt = patchDir / "dlc_base.txt";
  uint64_t baseEnd = 0;
  {
    std::ifstream in(baseTxt);
    unsigned long long v = 0;
    if (in >> v) baseEnd = v;
  }
  const uint64_t arcSize = fs::file_size(arcPath);
  if (!baseEnd || baseEnd > arcSize) baseEnd = align_up(arcSize, 2048);
  fs::path backup = patchDir / "filelist_patch_u.x360.bin.before_dlc";
  if (!fs::exists(backup)) fs::copy_file(listPath, backup);
  std::set<std::pair<uint8_t, uint32_t>> added;
  for (auto& j : jobs)
    if (j.good) added.insert({j.ue->ftype, j.ue->code});
  std::vector<PatchFile> files;
  int dropped = 0;
  for (auto& e : parse_filelist(read_file(listPath))) {
    if (e.sector * 2048 >= baseEnd || added.count({e.ftype, e.code})) { dropped++; continue; }
    files.push_back(PatchFile{e.code, e.ftype, e.path, e.sector, e.usize});
  }
  // list without the old DLC files first, so the list never names data past the end
  write_filelist(listPath, files);
  { std::ofstream(baseTxt) << baseEnd << "\n"; }
  fs::resize_file(arcPath, baseEnd);
  FILE* report = fopen((patchDir / "dlc_report.csv").string().c_str(), "w");
  if (report) fprintf(report, "pack,path,result,detail,size_360,size_steam,size_out\n");
  std::fstream arc(arcPath, std::ios::binary | std::ios::in | std::ios::out | std::ios::ate);
  if (!arc) throw std::runtime_error("cannot write " + arcPath.string());
  uint64_t written = baseEnd;
  int ok = 0, failed = 0;
  std::map<std::string, int> reasons;
  static const char zeros[2048] = {0};
  for (auto& j : jobs) {
    std::string why = j.why;
    for (auto& ch : why) if (ch == ',') ch = ';';
    if (!j.good) {
      failed++;
      reasons[j.why.size() > 60 ? j.why.substr(0, 60) : j.why]++;
      if (report) fprintf(report, "%s,%s,kept English,%s,%u,%u,\n", j.pack->num.c_str(), j.ue->path.c_str(), why.c_str(), j.ue->usize, j.je->usize);
      continue;
    }
    ok++;
    files.push_back(PatchFile{j.ue->code, j.ue->ftype, j.ue->path, written / 2048, uint32_t(j.ob.size())});
    arc.write((const char*)j.ob.data(), j.ob.size());
    written += j.ob.size();
    size_t pad = align_up(written, 2048) - written;
    arc.write(zeros, pad);
    written += pad;
    if (report) fprintf(report, "%s,%s,Japanese,,%u,%u,%zu\n", j.pack->num.c_str(), j.ue->path.c_str(), j.ue->usize, j.je->usize, j.ob.size());
  }
  arc.close();
  if (!arc) throw std::runtime_error("write failed: " + arcPath.string());
  if (report) fclose(report);
  write_filelist(listPath, files);
  printf("DLC voice lines: %d Japanese, %d kept English (%d earlier DLC entries replaced)\n", ok, failed, dropped);
  for (auto& r : reasons) printf("  kept English: %d x %s\n", r.second, r.first.c_str());
  printf("Patch now %zu files, %.0f MB\n", files.size(), written / 1048576.0);
  return ok ? 0 : 2;
}

#if !defined(LR_JPVOICE_NO_MAIN)  // lr_texdump.cpp includes this file for its archive reader
int main(int argc, char** argv) {
  // --ffmpeg <ffmpeg.exe>: re-encode the MS ADPCM voices to Ogg Vorbis (temp files next to out)
  std::vector<char*> args;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--ffmpeg") && i + 1 < argc) {
      g_ffmpeg = argv[++i];
      continue;
    }
    args.push_back(argv[i]);
  }
  argc = int(args.size());
  argv = args.data();
  g_tmp = fs::temp_directory_path() / ("lr_jpvoice_" + std::to_string(unsigned(time(nullptr))));
  if (!g_ffmpeg.empty()) fs::create_directories(g_tmp);
  struct TmpCleanup {
    ~TmpCleanup() {
      std::error_code ec;
      if (!g_tmp.empty()) fs::remove_all(g_tmp, ec);
    }
  } tmp_cleanup;
  try {
    if (argc == 5 && (!strcmp(argv[1], "--scd") || !strcmp(argv[1], "--wpd"))) {
      Bytes us = read_file(argv[2]), jp = read_file(argv[3]), out;
      std::string why;
      WpdStats st;
      bool ok = !strcmp(argv[1], "--scd") ? convert_scd(us, jp, out, why) : convert_wpd(us, jp, out, why, st);
      if (!ok) { fprintf(stderr, "failed: %s\n", why.c_str()); return 1; }
      if (st.members) printf("%d of %d members converted %s\n", st.converted, st.members, st.first_problem.c_str());
      write_file(argv[4], out);
      return 0;
    }
    if (argc == 6 && !strcmp(argv[1], "--lipsync")) {
      Bytes x = read_file(argv[2]), en = read_file(argv[3]), jp = read_file(argv[4]), out;
      std::string why;
      int taken = 0;
      if (!merge_jp_motion(x, en, jp, out, why, taken)) { fprintf(stderr, "failed: %s\n", why.c_str()); return 1; }
      printf("%d member(s) from the Japanese file\n", taken);
      write_file(argv[5], out);
      return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "--extract")) {
      std::vector<std::string> patterns(argv + 5, argv + argc);
      return extract(argv[2], argv[3], argv[4], patterns);
    }
    if (argc >= 5 && (!strcmp(argv[1], "--files") || !strcmp(argv[1], "--remove-files"))) {
      std::vector<fs::path> files(argv + 4, argv + argc);
      return patch_files(argv[2], argv[3], files, !strcmp(argv[1], "--remove-files"));
    }
    if (argc >= 5 && !strcmp(argv[1], "--dlc")) {
      std::vector<fs::path> contents(argv + 4, argv + argc);
      return add_dlc(argv[2], argv[3], contents);
    }
    if (argc == 3 && !strcmp(argv[1], "--list")) {
      for (auto& e : parse_filelist(read_file(argv[2])))
        printf("%u|%u|%llx:%x:%x:%s\n", e.code, e.ftype, (unsigned long long)e.sector, e.usize, e.csize, e.path.c_str());
      return 0;
    }
    if (argc == 4 || argc == 5) return build(argv[1], argv[2], argv[3], argc == 5 ? fs::path(argv[4]) : fs::path());
    fprintf(stderr, "usage: lr_jpvoice <360 game folder> <Steam game folder> <out folder> [samples folder]\n");
    return 1;
  } catch (const std::exception& ex) {
    fprintf(stderr, "error: %s\n", ex.what());
    return 1;
  }
}
#endif
