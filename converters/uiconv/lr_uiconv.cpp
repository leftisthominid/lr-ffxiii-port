// Lightning Returns port - builds the PS3 UI texture set.
//
// For every UI texture sheet (.xgr index + .imgb data) that exists in both the Xbox 360
// and the PS3 data, decodes each texture of both versions, and for the ones that differ
// (button icons, controller pictures, ...) writes the PS3 image as a DDS named after the
// hash the renderer computes for the 360 texture: XXH3-64 of its base level's blocks in
// row order (un-tiled, no padding, bytes not swapped). The renderer's "ps3ui" replacement set then shows the
// PS3 image wherever the game uses the 360 one, switchable while playing.
//
//   lr_uiconv <360 folder> <PS3 folder> <output folder>
// The two input folders hold the same relative names (<sheet>.xgr / <sheet>.imgb).
//
// 360 data is tiled (Xenos 2D tiling) with 16-bit (DXT) or 32-bit (8888) byte swapping.
// The PS3 layout is detected per texture: linear or swizzled, and the channel order, by
// trying each and keeping the one closest to the 360 image.
//
// Second job (--fonts, see the "--fonts" section below): 4x (or 2x/3x) replacements for the
// game's bitmap font atlases, reconstructed from the 360 textures, for the "replace" set:
//   lr_uiconv --fonts <ui_sheets.csv> <360 sys folder> <textures/replace/fonts> [--scale 4]

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <tuple>

// zlib inflate (RFC 1950/1951), for reading the game archives directly.
typedef std::vector<uint8_t> Bytes;
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

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

struct Tex {
  std::string name;
  uint32_t format = 0, mips = 0, kind = 0, width = 0, height = 0, depth = 0;
  uint32_t offset = 0, size = 0;  // base level in the .imgb
};

// .xgr: WPD container of TXBH entries, each with a GTEX header.
std::vector<Tex> ParseXgr(const std::vector<uint8_t>& d) {
  std::vector<Tex> out;
  if (d.size() < 16 || std::memcmp(d.data(), "WPD", 3) != 0) return out;
  uint32_t n = Be32(&d[4]);
  for (uint32_t i = 0; i < n && 16 + 32 * (i + 1) <= d.size(); ++i) {
    const uint8_t* e = &d[16 + 32 * i];
    char name[17] = {}, ext[9] = {};
    std::memcpy(name, e, 16);
    std::memcpy(ext, e + 24, 8);
    if (std::strcmp(ext, "txbh") != 0) continue;
    uint32_t off = Be32(e + 16), size = Be32(e + 20);
    if (uint64_t(off) + size > d.size()) continue;
    const uint8_t* t = &d[off];
    const uint8_t* g = nullptr;
    for (uint32_t k = 0; k + 4 <= size; ++k)
      if (std::memcmp(t + k, "GTEX", 4) == 0) { g = t + k; break; }
    if (!g || g + 24 > d.data() + d.size()) continue;
    Tex tex;
    tex.name = name;
    tex.format = g[6];
    tex.mips = g[7];
    tex.kind = g[9];
    tex.width = Be16(g + 10);
    tex.height = Be16(g + 12);
    tex.depth = Be16(g + 14);
    uint32_t mt = Be32(g + 16);
    if (g + mt + 8 > d.data() + d.size()) continue;
    tex.offset = Be32(g + mt);
    tex.size = Be32(g + mt + 4);
    out.push_back(tex);
  }
  return out;
}

struct FormatInfo {
  uint32_t block;  // texels per block side
  uint32_t bytes;  // bytes per block
  uint32_t dxgi;   // DXGI format of the replacement
};

bool GetFormat(uint32_t format, FormatInfo& fi) {
  switch (format) {
    case 3: case 4: fi = {1, 4, 28}; return true;   // A8R8G8B8 -> R8G8B8A8_UNORM
    case 24: fi = {4, 8, 71}; return true;          // DXT1 -> BC1
    case 25: fi = {4, 16, 74}; return true;         // DXT3 -> BC2
    case 26: fi = {4, 16, 77}; return true;         // DXT5 -> BC3
    default: return false;
  }
}

// Xenos 2D tiled address of block (x, y) in a surface `pitch` blocks wide (aligned to 32),
// in blocks.
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bytes_per_block) {
  uint32_t log_bpp = (bytes_per_block >> 2) + ((bytes_per_block >> 1) >> (bytes_per_block >> 2));
  uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log_bpp + 7);
  uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log_bpp;
  uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4);
  return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
         log_bpp;
}

// Linear block data (row-major, blocks_w x blocks_h) from the 360 base level. `raw` gets
// the blocks before byte swapping: the renderer's identifying hash is XXH3 of exactly that.
bool Untile360(const uint8_t* src, size_t src_size, const Tex& t, const FormatInfo& fi, std::vector<uint8_t>& out,
               std::vector<uint8_t>& raw) {
  uint32_t bw = (t.width + fi.block - 1) / fi.block, bh = (t.height + fi.block - 1) / fi.block;
  uint32_t pitch = (bw + 31) & ~31u;
  // GTEX byte 9 bit 2 set: tiled; clear: linear rows with the same 32-block pitch.
  bool tiled = (t.kind & 4) != 0;
  out.assign(size_t(bw) * bh * fi.bytes, 0);
  for (uint32_t y = 0; y < bh; ++y)
    for (uint32_t x = 0; x < bw; ++x) {
      size_t s = size_t(tiled ? TiledOffset(x, y, pitch, fi.bytes) : y * pitch + x) * fi.bytes;
      if (s + fi.bytes > src_size) return false;
      uint8_t* d = &out[(size_t(y) * bw + x) * fi.bytes];
      std::memcpy(d, src + s, fi.bytes);
    }
  raw = out;
  if (fi.block == 4) {  // 8-in-16 byte order
    for (size_t k = 0; k + 1 < out.size(); k += 2) std::swap(out[k], out[k + 1]);
  }
  return true;
}

void Color565(uint16_t c, uint8_t* rgb) {
  rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  rgb[2] = uint8_t((c & 31) * 255 / 31);
}

// Decodes linear BC1/BC2/BC3 blocks to RGBA8.
void DecodeDxt(const std::vector<uint8_t>& blocks, uint32_t w, uint32_t h, uint32_t format, std::vector<uint8_t>& rgba) {
  uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bytes = format == 24 ? 8 : 16;
  rgba.assign(size_t(w) * h * 4, 0);
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint8_t* b = &blocks[(size_t(by) * bw + bx) * bytes];
      const uint8_t* c = format == 24 ? b : b + 8;
      uint16_t c0 = uint16_t(c[0] | c[1] << 8), c1 = uint16_t(c[2] | c[3] << 8);
      uint8_t pal[4][4];
      Color565(c0, pal[0]);
      Color565(c1, pal[1]);
      pal[0][3] = pal[1][3] = 255;
      bool four = format != 24 || c0 > c1;
      for (int k = 0; k < 3; ++k) {
        pal[2][k] = four ? uint8_t((2 * pal[0][k] + pal[1][k]) / 3) : uint8_t((pal[0][k] + pal[1][k]) / 2);
        pal[3][k] = four ? uint8_t((pal[0][k] + 2 * pal[1][k]) / 3) : 0;
      }
      pal[2][3] = 255;
      pal[3][3] = four ? 255 : 0;
      uint32_t idx = uint32_t(c[4]) | uint32_t(c[5]) << 8 | uint32_t(c[6]) << 16 | uint32_t(c[7]) << 24;
      uint8_t alpha[16];
      if (format == 25) {
        for (int i = 0; i < 16; ++i) alpha[i] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
      } else if (format == 26) {
        uint8_t a0 = b[0], a1 = b[1], ap[8] = {a0, a1};
        if (a0 > a1) for (int i = 1; i < 7; ++i) ap[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
        else { for (int i = 1; i < 5; ++i) ap[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5); ap[6] = 0; ap[7] = 255; }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) alpha[i] = ap[(bits >> (3 * i)) & 7];
      }
      for (int py = 0; py < 4; ++py)
        for (int px = 0; px < 4; ++px) {
          uint32_t x = bx * 4 + px, y = by * 4 + py;
          if (x >= w || y >= h) continue;
          int i = py * 4 + px;
          uint8_t* d = &rgba[(size_t(y) * w + x) * 4];
          std::memcpy(d, pal[(idx >> (2 * i)) & 3], 4);
          if (format != 24) d[3] = alpha[i];
        }
    }
}

// Morton (swizzled) index for power-of-two textures, as the PS3 RSX lays them out.
uint32_t MortonIndex(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  uint32_t out = 0, bit = 0;
  uint32_t mw = w >> 1, mh = h >> 1;
  while (mw || mh) {
    if (mw) { out |= (x & 1) << bit++; x >>= 1; mw >>= 1; }
    if (mh) { out |= (y & 1) << bit++; y >>= 1; mh >>= 1; }
  }
  return out;
}

bool IsPow2(uint32_t v) { return v && !(v & (v - 1)); }

// Fraction of pixels that differ visibly (any channel by more than 24, ignoring pixels
// that are transparent in both).
double Difference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t pixels) {
  if (a.size() < pixels * 4 || b.size() < pixels * 4) return 1.0;
  size_t diff = 0;
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t* p = &a[i * 4];
    const uint8_t* q = &b[i * 4];
    if (p[3] < 8 && q[3] < 8) continue;
    int m = 0;
    for (int k = 0; k < 4; ++k) m = std::max(m, std::abs(int(p[k]) - int(q[k])));
    if (m > 24) ++diff;
  }
  return double(diff) / double(pixels ? pixels : 1);
}

struct Candidate {
  std::vector<uint8_t> rgba;    // decoded
  std::vector<uint8_t> blocks;  // linear standard blocks (DXT) for the DDS
  double diff = 2.0;
};

// Decodes the other platform's base level, trying the layouts it may use: tight rows or
// rows padded to the pitch implied by the stored level size, linear or swizzled (8888),
// and the channel order / 16-bit byte order.
Candidate DecodeOther(const uint8_t* src, size_t src_size, const Tex& t, const FormatInfo& fi,
                      const std::vector<uint8_t>& reference) {
  Candidate best;
  size_t pixels = size_t(t.width) * t.height;
  uint32_t bw = (t.width + fi.block - 1) / fi.block, bh = (t.height + fi.block - 1) / fi.block;
  size_t tight = size_t(bw) * fi.bytes;
  std::vector<size_t> pitches = {tight};
  if (t.size && bh && t.size % bh == 0 && t.size / bh > tight) pitches.push_back(t.size / bh);
  for (size_t pitch : pitches) {
    if (src_size < pitch * (bh - 1) + tight) continue;
    if (fi.block == 4) {
      for (int swap = 0; swap < 2; ++swap) {
        Candidate c;
        c.blocks.resize(tight * bh);
        for (uint32_t y = 0; y < bh; ++y) std::memcpy(&c.blocks[y * tight], src + y * pitch, tight);
        if (swap) for (size_t k = 0; k + 1 < c.blocks.size(); k += 2) std::swap(c.blocks[k], c.blocks[k + 1]);
        DecodeDxt(c.blocks, t.width, t.height, t.format, c.rgba);
        c.diff = Difference(c.rgba, reference, pixels);
        if (c.diff < best.diff) best = std::move(c);
      }
      continue;
    }
    static const std::array<std::array<int, 4>, 4> orders = {{
        {1, 2, 3, 0},  // A R G B in memory -> R G B A
        {2, 1, 0, 3},  // B G R A
        {0, 1, 2, 3},  // R G B A
        {3, 2, 1, 0},  // A B G R
    }};
    for (int swizzled = 0; swizzled < 2; ++swizzled) {
      if (swizzled && (pitch != tight || !(IsPow2(t.width) && IsPow2(t.height)))) continue;
      for (const auto& o : orders) {
        Candidate c;
        c.rgba.resize(pixels * 4);
        for (uint32_t y = 0; y < t.height; ++y)
          for (uint32_t x = 0; x < t.width; ++x) {
            const uint8_t* p = swizzled ? src + size_t(MortonIndex(x, y, t.width, t.height)) * 4
                                        : src + y * pitch + size_t(x) * 4;
            uint8_t* d = &c.rgba[(size_t(y) * t.width + x) * 4];
            for (int k = 0; k < 4; ++k) d[k] = p[o[k]];
          }
        c.diff = Difference(c.rgba, reference, pixels);
        if (c.diff < best.diff) best = std::move(c);
      }
    }
  }
  return best;
}

bool WriteDds(const fs::path& path, uint32_t w, uint32_t h, uint32_t dxgi, const std::vector<uint8_t>& data) {
  uint32_t header[31] = {};
  header[0] = 124;
  header[1] = 0x1 | 0x2 | 0x4 | 0x1000;  // caps | height | width | pixelformat
  header[2] = h;
  header[3] = w;
  header[4] = dxgi == 28 ? w * 4 : uint32_t(data.size());
  header[6] = 1;  // mip count
  header[18] = 32;  // pixel format size
  header[19] = 0x4;  // FOURCC
  header[20] = 0x30315844;  // 'DX10'
  header[26] = 0x1000;  // caps: texture
  uint32_t dx10[5] = {dxgi, 3 /* 2D */, 0, 1, 0};
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write("DDS ", 4);
  f.write(reinterpret_cast<const char*>(header), sizeof(header));
  f.write(reinterpret_cast<const char*>(dx10), sizeof(dx10));
  f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  return bool(f);
}

// Content signature of a 360 texture (RGBA, decoded), written next to its replacement as
// <name>.sig. The renderer uses it when its hash of the game's texture does not find the
// replacement (see the renderer's replacement.h; the cell layout must stay identical).
void WriteSignature(const fs::path& dds_path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  constexpr uint32_t kCells = 16;
  uint8_t sig[kCells * kCells * 4];
  for (uint32_t cy = 0; cy < kCells; ++cy) {
    uint32_t y0 = cy * height / kCells, y1 = std::min(std::max(y0 + 1, (cy + 1) * height / kCells), height);
    for (uint32_t cx = 0; cx < kCells; ++cx) {
      uint32_t x0 = cx * width / kCells, x1 = std::min(std::max(x0 + 1, (cx + 1) * width / kCells), width);
      uint64_t sum[4] = {}, n = 0;
      for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x, ++n)
          for (int k = 0; k < 4; ++k) sum[k] += rgba[(size_t(y) * width + x) * 4 + k];
      for (int k = 0; k < 4; ++k) sig[(cy * kCells + cx) * 4 + k] = uint8_t(n ? (sum[k] + n / 2) / n : 0);
    }
  }
  fs::path path = dds_path;
  path.replace_extension(".sig");
  std::ofstream f(path, std::ios::binary);
  uint32_t header[3] = {1, width, height};
  f.write("LRSG", 4);
  f.write(reinterpret_cast<const char*>(header), sizeof(header));
  f.write(reinterpret_cast<const char*>(sig), sizeof(sig));
}

}  // namespace

// Reads one archive entry (sector, size, stored size) the way the game does.
struct ArchiveSet {
  fs::path dir;
  std::string suffix;  // ".x360.bin" / ".ps3.bin"
  std::map<std::string, std::unique_ptr<std::ifstream>> files;
  Bytes read(const std::string& arc, uint64_t sector, uint32_t size, uint32_t csize) {
    auto& f = files[arc];
    if (!f) {
      f = std::make_unique<std::ifstream>(dir / (arc + suffix), std::ios::binary);
      if (!*f) throw std::runtime_error("cannot open " + (dir / (arc + suffix)).string());
    }
    Bytes raw(csize);
    f->clear();
    f->seekg(std::streamoff(sector * 2048));
    f->read(reinterpret_cast<char*>(raw.data()), csize);
    if (!*f) throw std::runtime_error("short read in " + arc + suffix);
    if (csize == size) return raw;
    Bytes out;
    out.reserve(size);
    inflate::zlib(raw.data(), raw.size(), out);
    if (out.size() != size) throw std::runtime_error("bad size after inflate in " + arc + suffix);
    return out;
  }
};

// ---------------------------------------------------------------------------------------------
// --fonts: high-resolution replacements for the game's bitmap fonts.
//
// The game draws all menu / dialogue / subtitle text from three glyph atlases in
// gui/resident/system_jp.x360.xgr (used by every language): wfnt16 (1024x1024), wfnt18 and
// wfnt28 (2048x2048), all DXT1 with white/black endpoints, i.e. 2-bit anti-aliased coverage.
// The 14-point font reuses the wfnt28 atlas. Each has a .wfl glyph table (wfnt16t, ...). A few
// number sheets in gui/resident/system (btl_number01/02, w_num) are coverage art too.
// Glyph UVs are normalized and the shaders sample with normalized coordinates (outline taps
// are offset by a guest-supplied 1/size constant), so a same-aspect larger texture drops in.
//
// Since the original vector font is not available, the glyph shapes are reconstructed from the
// coverage: an anti-aliased signed distance field (Gustavson & Strand 2011: the coverage of an
// edge texel places the edge inside it) is computed per channel, upsampled with a cubic filter
// and re-thresholded with a smoothstep about one screen pixel wide. Output: BC1/BC2/BC3 (the
// format the renderer uses for the guest texture) with a full mip chain, named after the hash
// of the 360 texture like the PS3 UI set.
namespace fontup {

struct Plane {
  int w = 0, h = 0;
  std::vector<float> v;
  Plane() = default;
  Plane(int w_, int h_, float fill = 0.f) : w(w_), h(h_), v(size_t(w_) * h_, fill) {}
  float& at(int x, int y) { return v[size_t(y) * w + x]; }
  float at(int x, int y) const { return v[size_t(y) * w + x]; }
  float clamped(int x, int y) const { return at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1)); }
};

// Distance from a texel centre to a straight edge crossing the texel, given the texel's coverage
// `a` and the edge normal (gx, gy); positive when the centre is outside.
float EdgeDistance(float gx, float gy, float a) {
  if (gx == 0.f && gy == 0.f) return 0.5f - a;
  float len = std::sqrt(gx * gx + gy * gy);
  gx = std::fabs(gx / len);
  gy = std::fabs(gy / len);
  if (gx < gy) std::swap(gx, gy);
  float a1 = 0.5f * gy / gx;
  if (a < a1) return 0.5f * (gx + gy) - std::sqrt(2.f * gx * gy * a);
  if (a < 1.f - a1) return (0.5f - a) * gx;
  return -0.5f * (gx + gy) + std::sqrt(2.f * gx * gy * (1.f - a));
}

// Signed distance (texels, positive outside) from every texel centre to the 0.5 contour of the
// coverage. Exact search within `radius` texels of an edge; clamped to +-radius beyond.
Plane SignedDistance(const Plane& c, int radius) {
  const int w = c.w, h = c.h;
  const float s2 = std::sqrt(2.f), cap = float(radius);
  std::vector<float> gx(size_t(w) * h, 0.f), gy(size_t(w) * h, 0.f);
  std::vector<uint8_t> boundary(size_t(w) * h, 0);
  auto cls = [](float a) { return a <= 0.f ? 0 : a >= 1.f ? 2 : 1; };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float a = c.at(x, y);
      int k = cls(a);
      bool b = k == 1;
      for (int dy = -1; dy <= 1 && !b; ++dy)
        for (int dx = -1; dx <= 1 && !b; ++dx) b = cls(c.clamped(x + dx, y + dy)) != k;
      boundary[size_t(y) * w + x] = b;
      if (k != 1) continue;
      gx[size_t(y) * w + x] = -c.clamped(x - 1, y - 1) - s2 * c.clamped(x - 1, y) - c.clamped(x - 1, y + 1) +
                              c.clamped(x + 1, y - 1) + s2 * c.clamped(x + 1, y) + c.clamped(x + 1, y + 1);
      gy[size_t(y) * w + x] = -c.clamped(x - 1, y - 1) - s2 * c.clamped(x, y - 1) - c.clamped(x + 1, y - 1) +
                              c.clamped(x - 1, y + 1) + s2 * c.clamped(x, y + 1) + c.clamped(x + 1, y + 1);
    }
  // Texels with a boundary texel within the search radius (separable dilation).
  std::vector<uint8_t> near_row(size_t(w) * h, 0), near(size_t(w) * h, 0);
  for (int y = 0; y < h; ++y) {
    int last = -1000000;
    for (int x = 0; x < w; ++x) {
      if (boundary[size_t(y) * w + x]) last = x;
      if (x - last <= radius) near_row[size_t(y) * w + x] = 1;
    }
    last = 1000000;
    for (int x = w - 1; x >= 0; --x) {
      if (boundary[size_t(y) * w + x]) last = x;
      if (last - x <= radius) near_row[size_t(y) * w + x] = 1;
    }
  }
  for (int x = 0; x < w; ++x) {
    int last = -1000000;
    for (int y = 0; y < h; ++y) {
      if (near_row[size_t(y) * w + x]) last = y;
      if (y - last <= radius) near[size_t(y) * w + x] = 1;
    }
    last = 1000000;
    for (int y = h - 1; y >= 0; --y) {
      if (near_row[size_t(y) * w + x]) last = y;
      if (last - y <= radius) near[size_t(y) * w + x] = 1;
    }
  }
  Plane out(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      size_t i = size_t(y) * w + x;
      float a = c.v[i];
      if (!near[i]) {
        out.v[i] = a >= 0.5f ? -cap : cap;
        continue;
      }
      // Distance to the shape (dout) and to its complement (din); the texel itself counts.
      float dout = a > 0.f ? EdgeDistance(gx[i], gy[i], a) : cap + 1.f;
      float din = a < 1.f ? EdgeDistance(gx[i], gy[i], 1.f - a) : cap + 1.f;
      for (int dy = -radius; dy <= radius; ++dy) {
        int qy = y + dy;
        if (qy < 0 || qy >= h) continue;
        for (int dx = -radius; dx <= radius; ++dx) {
          int qx = x + dx;
          if (qx < 0 || qx >= w || (dx == 0 && dy == 0)) continue;
          size_t q = size_t(qy) * w + qx;
          if (!boundary[q]) continue;
          float d2 = float(dx * dx + dy * dy);
          if (d2 > (cap + 1.f) * (cap + 1.f)) continue;
          float di = std::sqrt(d2), aq = c.v[q];
          // The direction to the edge texel stands in for the edge normal (edtaa).
          if (aq > 0.f) dout = std::min(dout, di + EdgeDistance(float(dx), float(dy), aq));
          if (aq < 1.f) din = std::min(din, di + EdgeDistance(float(dx), float(dy), 1.f - aq));
        }
      }
      out.v[i] = std::clamp(std::max(dout, 0.f) - std::max(din, 0.f), -cap, cap);
    }
  return out;
}

// 3x3 binomial blur of the distance field (--smooth): evens out the wobble the 2-bit coverage
// leaves in edge positions, but it also flattens the distance ridges along stroke centres and in
// counters, so thin strokes get thinner and small counters close up. Off by default.
Plane Smooth(const Plane& p) {
  Plane t(p.w, p.h), o(p.w, p.h);
  for (int y = 0; y < p.h; ++y)
    for (int x = 0; x < p.w; ++x)
      t.at(x, y) = 0.25f * p.clamped(x - 1, y) + 0.5f * p.at(x, y) + 0.25f * p.clamped(x + 1, y);
  for (int y = 0; y < p.h; ++y)
    for (int x = 0; x < p.w; ++x)
      o.at(x, y) = 0.25f * t.clamped(x, y - 1) + 0.5f * t.at(x, y) + 0.25f * t.clamped(x, y + 1);
  return o;
}

// Cubic upsampling by an integer factor (texel centres aligned). bspline = smoothing B-spline,
// else Catmull-Rom (interpolating).
Plane Upsample(const Plane& p, int scale, bool bspline) {
  auto weights = [&](float t, float* wt) {
    float t2 = t * t, t3 = t2 * t;
    if (bspline) {
      wt[0] = (1 - t) * (1 - t) * (1 - t) / 6.f;
      wt[1] = (3 * t3 - 6 * t2 + 4) / 6.f;
      wt[2] = (-3 * t3 + 3 * t2 + 3 * t + 1) / 6.f;
      wt[3] = t3 / 6.f;
    } else {
      wt[0] = 0.5f * (-t3 + 2 * t2 - t);
      wt[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
      wt[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
      wt[3] = 0.5f * (t3 - t2);
    }
  };
  std::vector<int> base(scale);
  std::vector<std::array<float, 4>> wts(scale);
  for (int k = 0; k < scale; ++k) {
    float s = (k + 0.5f) / scale - 0.5f;
    int b = int(std::floor(s));
    base[k] = b;
    weights(s - b, wts[k].data());
  }
  const int W = p.w * scale, H = p.h * scale;
  Plane t(W, p.h), o(W, H);
  for (int y = 0; y < p.h; ++y)
    for (int X = 0; X < W; ++X) {
      int x0 = X / scale + base[X % scale] - 1;
      const float* wt = wts[X % scale].data();
      float s = 0;
      for (int i = 0; i < 4; ++i) s += wt[i] * p.clamped(x0 + i, y);
      t.at(X, y) = s;
    }
  for (int Y = 0; Y < H; ++Y) {
    int y0 = Y / scale + base[Y % scale] - 1;
    const float* wt = wts[Y % scale].data();
    for (int X = 0; X < W; ++X) {
      float s = 0;
      for (int i = 0; i < 4; ++i) s += wt[i] * t.clamped(X, y0 + i);
      o.at(X, Y) = s;
    }
  }
  return o;
}

struct Options {
  int scale = 4;
  float edge = 0;  // smoothstep width in output texels; 0 = scale / 2 (min 1)
  bool smooth = false, bspline = false, mips = true, preview = false;
};

// Coverage plane (0..1) -> reconstructed coverage at `scale` times the size.
Plane UpscaleCoverage(const Plane& c, const Options& opt) {
  const int scale = opt.scale, radius = 4;
  const float edge = opt.edge > 0 ? opt.edge : std::max(1.f, scale * 0.5f);
  Plane sdf = SignedDistance(c, radius);
  if (opt.smooth) sdf = Smooth(sdf);
  Plane up = Upsample(sdf, scale, opt.bspline);
  // Nothing may appear more than a texel away from where the original has coverage, so
  // neighbouring glyphs cannot bleed into each other.
  std::vector<uint8_t> support(size_t(c.w) * c.h, 0);
  for (int y = 0; y < c.h; ++y)
    for (int x = 0; x < c.w; ++x) {
      bool s = false;
      for (int dy = -1; dy <= 1 && !s; ++dy)
        for (int dx = -1; dx <= 1 && !s; ++dx) {
          int qx = x + dx, qy = y + dy;
          s = qx >= 0 && qy >= 0 && qx < c.w && qy < c.h && c.at(qx, qy) > 0.f;
        }
      support[size_t(y) * c.w + x] = s;
    }
  for (int Y = 0; Y < up.h; ++Y)
    for (int X = 0; X < up.w; ++X) {
      float& v = up.at(X, Y);
      if (!support[size_t(Y / scale) * c.w + X / scale]) {
        v = 0.f;
        continue;
      }
      float t = std::clamp(0.5f - v * scale / edge, 0.f, 1.f);
      v = t * t * (3.f - 2.f * t);
    }
  return up;
}

Plane UpscaleBicubic(const Plane& c, int scale) {
  Plane up = Upsample(c, scale, false);
  for (float& v : up.v) v = std::clamp(v, 0.f, 1.f);
  return up;
}

Plane Downsample(const Plane& p) {
  Plane o(std::max(1, p.w / 2), std::max(1, p.h / 2));
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x)
      o.at(x, y) = 0.25f * (p.clamped(2 * x, 2 * y) + p.clamped(2 * x + 1, 2 * y) + p.clamped(2 * x, 2 * y + 1) +
                            p.clamped(2 * x + 1, 2 * y + 1));
  return o;
}

// --- block compression ---

uint16_t Quantize565(const float* rgb) {
  int r = int(std::lround(std::clamp(rgb[0], 0.f, 1.f) * 31.f));
  int g = int(std::lround(std::clamp(rgb[1], 0.f, 1.f) * 63.f));
  int b = int(std::lround(std::clamp(rgb[2], 0.f, 1.f) * 31.f));
  return uint16_t(r << 11 | g << 5 | b);
}
void Expand565(uint16_t c, float* rgb) {
  rgb[0] = float(((c >> 11) & 31) * 255 / 31) / 255.f;
  rgb[1] = float(((c >> 5) & 63) * 255 / 63) / 255.f;
  rgb[2] = float((c & 31) * 255 / 31) / 255.f;
}

// BC1 colour block from 16 RGBA texels (0..1). four_only: BC2/BC3 colour (always 4 colours);
// otherwise texels with alpha < 0.5 use BC1's transparent index.
void EncodeColorBlock(const float (*px)[4], bool four_only, uint8_t* out) {
  bool transparent = false;
  float mean[3] = {0, 0, 0};
  int n = 0;
  for (int i = 0; i < 16; ++i) {
    if (!four_only && px[i][3] < 0.5f) { transparent = true; continue; }
    for (int k = 0; k < 3; ++k) mean[k] += px[i][k];
    ++n;
  }
  uint16_t c0 = 0, c1 = 0;
  uint32_t indices = 0;
  if (!n) {  // fully transparent
    c0 = 0; c1 = 0xFFFF; indices = 0xFFFFFFFFu;
  } else {
    for (float& m : mean) m /= float(n);
    // Principal axis (power iteration); grey data gives (1,1,1).
    float cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
      if (!four_only && px[i][3] < 0.5f) continue;
      float d[3] = {px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2]};
      cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2];
      cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
    }
    float axis[3] = {1, 1, 1};
    for (int it = 0; it < 8; ++it) {
      float a[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                    cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                    cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
      float len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
      if (len < 1e-12f) break;
      for (int k = 0; k < 3; ++k) axis[k] = a[k] / len;
    }
    float tmin = 1e9f, tmax = -1e9f;
    for (int i = 0; i < 16; ++i) {
      if (!four_only && px[i][3] < 0.5f) continue;
      float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
      tmin = std::min(tmin, t);
      tmax = std::max(tmax, t);
    }
    float e_hi[3], e_lo[3];
    for (int k = 0; k < 3; ++k) {
      e_hi[k] = mean[k] + tmax * axis[k];
      e_lo[k] = mean[k] + tmin * axis[k];
    }
    c0 = Quantize565(e_hi);
    c1 = Quantize565(e_lo);
    // 4-colour mode needs c0 > c1, 3-colour (transparency) c0 <= c1.
    bool three = transparent;
    if (three ? c0 > c1 : c0 < c1) std::swap(c0, c1);
    float pal[4][3];
    Expand565(c0, pal[0]);
    Expand565(c1, pal[1]);
    int count = 4;
    if (c0 == c1) {
      count = 1;
    } else if (three) {
      for (int k = 0; k < 3; ++k) pal[2][k] = (pal[0][k] + pal[1][k]) * 0.5f;
      count = 3;
    } else {
      for (int k = 0; k < 3; ++k) {
        pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3.f;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3.f;
      }
    }
    for (int i = 0; i < 16; ++i) {
      uint32_t best = 0;
      if (!four_only && px[i][3] < 0.5f) {
        best = 3;
      } else {
        float bd = 1e9f;
        for (int k = 0; k < count; ++k) {
          float d = 0;
          for (int j = 0; j < 3; ++j) d += (px[i][j] - pal[k][j]) * (px[i][j] - pal[k][j]);
          if (d < bd) { bd = d; best = uint32_t(k); }
        }
      }
      indices |= best << (2 * i);
    }
  }
  out[0] = uint8_t(c0); out[1] = uint8_t(c0 >> 8);
  out[2] = uint8_t(c1); out[3] = uint8_t(c1 >> 8);
  for (int k = 0; k < 4; ++k) out[4 + k] = uint8_t(indices >> (8 * k));
}

void EncodeAlphaBlock(const float (*px)[4], uint8_t* out) {  // BC3 alpha
  int lo = 255, hi = 0, a[16];
  for (int i = 0; i < 16; ++i) {
    a[i] = int(std::lround(std::clamp(px[i][3], 0.f, 1.f) * 255.f));
    lo = std::min(lo, a[i]);
    hi = std::max(hi, a[i]);
  }
  out[0] = uint8_t(hi);
  out[1] = uint8_t(lo);
  uint64_t bits = 0;
  if (hi != lo) {
    int pal[8] = {hi, lo};
    for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * hi + i * lo) / 7;
    for (int i = 0; i < 16; ++i) {
      int best = 0, bd = 1 << 30;
      for (int k = 0; k < 8; ++k)
        if (std::abs(a[i] - pal[k]) < bd) { bd = std::abs(a[i] - pal[k]); best = k; }
      bits |= uint64_t(best) << (3 * i);
    }
  }
  for (int k = 0; k < 6; ++k) out[2 + k] = uint8_t(bits >> (8 * k));
}

// RGBA planes -> one mip level in the replacement's DXGI format.
// A constant channel is a 1x1 plane.
std::vector<uint8_t> EncodeLevel(const Plane* const* ch, int w, int h, uint32_t dxgi) {
  std::vector<uint8_t> out;
  if (dxgi == 28) {
    out.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        for (int k = 0; k < 4; ++k)
          out[(size_t(y) * w + x) * 4 + k] = uint8_t(std::lround(std::clamp(ch[k]->clamped(x, y), 0.f, 1.f) * 255.f));
    return out;
  }
  const int bw = (w + 3) / 4, bh = (h + 3) / 4, bytes = dxgi == 71 ? 8 : 16;
  out.resize(size_t(bw) * bh * bytes);
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      float px[16][4];
      for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 4; ++k) px[i][k] = ch[k]->clamped(bx * 4 + i % 4, by * 4 + i / 4);
      uint8_t* b = &out[(size_t(by) * bw + bx) * bytes];
      if (dxgi == 71) {
        EncodeColorBlock(px, false, b);
      } else {
        if (dxgi == 74) {  // BC2: explicit 4-bit alpha
          for (int i = 0; i < 16; i += 2) {
            int a0 = int(std::lround(std::clamp(px[i][3], 0.f, 1.f) * 15.f));
            int a1 = int(std::lround(std::clamp(px[i + 1][3], 0.f, 1.f) * 15.f));
            b[i / 2] = uint8_t(a0 | a1 << 4);
          }
        } else {
          EncodeAlphaBlock(px, b);
        }
        EncodeColorBlock(px, true, b + 8);
      }
    }
  return out;
}

bool WriteDdsLevels(const fs::path& path, uint32_t w, uint32_t h, uint32_t dxgi,
                    const std::vector<std::vector<uint8_t>>& levels) {
  uint32_t header[31] = {};
  header[0] = 124;
  header[1] = 0x1 | 0x2 | 0x4 | 0x1000 | (levels.size() > 1 ? 0x20000 : 0);
  header[2] = h;
  header[3] = w;
  header[4] = uint32_t(levels[0].size());
  header[6] = uint32_t(levels.size());
  header[18] = 32;
  header[19] = 0x4;
  header[20] = 0x30315844;  // 'DX10'
  header[26] = 0x1000 | (levels.size() > 1 ? 0x400008 : 0);
  uint32_t dx10[5] = {dxgi, 3, 0, 1, 0};
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write("DDS ", 4);
  f.write(reinterpret_cast<const char*>(header), sizeof(header));
  f.write(reinterpret_cast<const char*>(dx10), sizeof(dx10));
  for (const auto& l : levels) f.write(reinterpret_cast<const char*>(l.data()), std::streamsize(l.size()));
  return bool(f);
}

// Minimal PNG writer (8-bit grey or RGB, stored deflate blocks) for the previews.
bool WritePng(const fs::path& path, int w, int h, int channels, const std::vector<uint8_t>& px) {
  static uint32_t crc_table[256];
  if (!crc_table[1])
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      crc_table[n] = c;
    }
  auto be32 = [](std::vector<uint8_t>& v, uint32_t x) {
    for (int k = 3; k >= 0; --k) v.push_back(uint8_t(x >> (8 * k)));
  };
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (size_t(w) * channels + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), px.begin() + size_t(y) * w * channels, px.begin() + size_t(y + 1) * w * channels);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t s1 = 1, s2 = 0;
  for (uint8_t b : raw) { s1 = (s1 + b) % 65521; s2 = (s2 + s1) % 65521; }
  for (size_t pos = 0; pos < raw.size();) {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    z.push_back(pos + n >= raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  be32(z, s2 << 16 | s1);
  std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    be32(file, uint32_t(data.size()));
    size_t start = file.size();
    file.insert(file.end(), type, type + 4);
    file.insert(file.end(), data.begin(), data.end());
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = start; i < file.size(); ++i) c = crc_table[(c ^ file[i]) & 0xFF] ^ (c >> 8);
    be32(file, c ^ 0xFFFFFFFFu);
  };
  std::vector<uint8_t> ihdr;
  be32(ihdr, uint32_t(w));
  be32(ihdr, uint32_t(h));
  ihdr.insert(ihdr.end(), {8, uint8_t(channels == 3 ? 2 : 0), 0, 0, 0});
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
  return bool(f);
}

// WPD entry (name, extension) -> bytes inside the .xgr.
bool FindWpdEntry(const Bytes& d, const std::string& want_name, const std::string& want_ext, const uint8_t*& p,
                  uint32_t& size) {
  if (d.size() < 16 || std::memcmp(d.data(), "WPD", 3) != 0) return false;
  uint32_t n = Be32(&d[4]);
  for (uint32_t i = 0; i < n && 16 + 32 * (i + 1) <= d.size(); ++i) {
    const uint8_t* e = &d[16 + 32 * i];
    char name[17] = {}, ext[9] = {};
    std::memcpy(name, e, 16);
    std::memcpy(ext, e + 24, 8);
    uint32_t off = Be32(e + 16), sz = Be32(e + 20);
    if (want_name == name && want_ext == ext && uint64_t(off) + sz <= d.size()) {
      p = &d[off];
      size = sz;
      return true;
    }
  }
  return false;
}

// Glyph table (.wfl, little-endian): header {30, size, line height, padding, kerning pairs,
// ascent?, ?, ?}; Latin-1 metrics [224] {left (int8), width, right (int8), 0}; kerning pairs
// {char1, char2, adjust (int8), 0}; Latin-1 atlas positions [224] {u16 x, u16 y}; then the
// metrics / code map / positions the Japanese and Asian languages use. Grid glyphs (kana,
// kanji) sit in cells of (size + padding) texels starting at `padding`.
struct GlyphTable {
  uint32_t size = 0, line = 0, pad = 0, kerns = 0;
  const uint8_t* metrics = nullptr;
  const uint8_t* kern = nullptr;
  const uint8_t* pos = nullptr;
};
bool ParseWfl(const uint8_t* p, uint32_t size, GlyphTable& t) {
  auto le32 = [&](int i) { return uint32_t(p[4 * i]) | uint32_t(p[4 * i + 1]) << 8 | uint32_t(p[4 * i + 2]) << 16 |
                                  uint32_t(p[4 * i + 3]) << 24; };
  if (size < 32) return false;
  t.size = le32(1); t.line = le32(2); t.pad = le32(3); t.kerns = le32(4);
  if (!t.size || !t.line || t.kerns > 4096 || 32 + 896 + t.kerns * 4 + 896 > size) return false;
  t.metrics = p + 32;
  t.kern = t.metrics + 896;
  t.pos = t.kern + t.kerns * 4;
  return true;
}

// Bilinear sample of a plane at texel coordinates (texel centres at +0.5).
float Bilinear(const Plane& p, float u, float v) {
  u -= 0.5f; v -= 0.5f;
  int x = int(std::floor(u)), y = int(std::floor(v));
  float fx = u - x, fy = v - y;
  return (1 - fy) * ((1 - fx) * p.clamped(x, y) + fx * p.clamped(x + 1, y)) +
         fy * ((1 - fx) * p.clamped(x, y + 1) + fx * p.clamped(x + 1, y + 1));
}

// Renders a sample line with the glyph table from each atlas version, as the game would draw
// it with `zoom` screen pixels per guest texel (2 = the 2560x1440 internal resolution) and
// bilinear filtering: original, plain bicubic upscale, reconstruction.
void PreviewText(const fs::path& path, const GlyphTable& t, const Plane* const* atlases, const int* scales, int count,
                 int zoom) {
  const char* text = "Lightning Returns: Hope's Ark, 13 days. Quick brown fox jumps over the lazy dog! 0123456789";
  int width = 8;
  int prev = 0;
  for (const char* s = text; *s; ++s) {
    int c = uint8_t(*s);
    if (c < 32 || c > 255) continue;
    const uint8_t* m = t.metrics + 4 * (c - 32);
    width += int8_t(m[0]) + m[1] + int8_t(m[2]);
    prev = c;
  }
  (void)prev;
  const int row_h = int(t.line) * zoom + 8, W = width * zoom, H = row_h * count;
  std::vector<uint8_t> img(size_t(W) * H, 0);
  for (int r = 0; r < count; ++r) {
    const Plane& a = *atlases[r];
    float sc = float(scales[r]);
    int pen = 4;
    prev = 0;
    for (const char* s = text; *s; ++s) {
      int c = uint8_t(*s);
      if (c < 32 || c > 255) continue;
      const uint8_t* m = t.metrics + 4 * (c - 32);
      for (uint32_t k = 0; k < t.kerns; ++k)
        if (t.kern[4 * k] == prev && t.kern[4 * k + 1] == c) pen += int8_t(t.kern[4 * k + 2]);
      int gx = t.pos[4 * (c - 32)] | t.pos[4 * (c - 32) + 1] << 8;
      int gy = t.pos[4 * (c - 32) + 2] | t.pos[4 * (c - 32) + 3] << 8;
      int x0 = (pen + int8_t(m[0])) * zoom, gw = m[1] * zoom;
      for (int y = 0; y < int(t.line) * zoom; ++y)
        for (int x = 0; x < gw; ++x) {
          float u = (gx + (x + 0.5f) / zoom) * sc, v = (gy + (y + 0.5f) / zoom) * sc;
          float cov = Bilinear(a, u, v);
          int ox = x0 + x, oy = r * row_h + 4 + y;
          if (ox < 0 || ox >= W) continue;
          uint8_t& o = img[size_t(oy) * W + ox];
          o = std::max<uint8_t>(o, uint8_t(std::lround(std::clamp(cov, 0.f, 1.f) * 255.f)));
        }
      pen += int8_t(m[0]) + m[1] + int8_t(m[2]);
      prev = c;
    }
  }
  WritePng(path, W, H, 1, img);
}

struct FontTarget {
  std::string sheet, texture;
};

int FontsMain(int argc, char** argv) {
  // lr_uiconv --fonts <ui_sheets.csv> <360 sys folder> <output folder> [options]
  // lr_uiconv --fonts-files <sheet.xgr> <sheet.imgb> <output folder> [options]
  bool files_mode = std::strcmp(argv[1], "--fonts-files") == 0;
  if (argc < 5) {
    std::fprintf(stderr,
                 "usage: lr_uiconv --fonts <ui_sheets.csv> <360 sys folder> <output folder> [options]\n"
                 "       lr_uiconv --fonts-files <sheet.xgr> <sheet.imgb> <output folder> [options]\n"
                 "options: --scale N (2..4, default 4)  --edge T (edge width in output texels, default scale/2)\n"
                 "         --no-mips  --smooth  --bspline  --preview  --tex NAME (texture to convert; repeatable;\n"
                 "         default: the font atlases, the number sheets and the loading lettering)  --sheet NAME (also read this sheet)\n");
    return 1;
  }
  Options opt;
  std::vector<std::string> textures, extra_sheets;
  for (int i = 5; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
      return argv[++i];
    };
    if (a == "--scale") opt.scale = std::stoi(next());
    else if (a == "--edge") opt.edge = std::stof(next());
    else if (a == "--no-mips") opt.mips = false;
    else if (a == "--smooth") opt.smooth = true;
    else if (a == "--bspline") opt.bspline = true;
    else if (a == "--preview") opt.preview = true;
    else if (a == "--tex") textures.push_back(next());
    else if (a == "--sheet") extra_sheets.push_back(next());
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
  }
  if (opt.scale < 2 || opt.scale > 4) { std::fprintf(stderr, "--scale must be 2, 3 or 4\n"); return 1; }
  if (textures.empty())
    textures = {"wfnt16", "wfnt18", "wfnt28", "wfnt16x", "wfnt18x", "wfnt28x", "btl_number01", "btl_number02", "w_num",
                "nlfont"};  // nlfont: the "NOW... LOADING" lettering
  fs::path out_dir = argv[4];
  fs::create_directories(out_dir);
  std::ofstream report(out_dir / "fonts_report.csv");
  report << "sheet,texture,format,width,height,out_width,out_height,mips,hash360,file\n";

  // (sheet name, xgr bytes, imgb bytes)
  std::vector<std::tuple<std::string, Bytes, Bytes>> sheets;
  if (files_mode) {
    fs::path xgr = argv[2], imgb = argv[3];
    std::string sheet = xgr.stem().string();
    if (sheet.size() > 5 && sheet.compare(sheet.size() - 5, 5, ".x360") == 0) sheet.resize(sheet.size() - 5);
    sheets.emplace_back(sheet, ReadFile(xgr), ReadFile(imgb));
  } else {
    std::vector<std::string> wanted = {"gui/resident/system_jp", "gui/resident/system"};
    wanted.insert(wanted.end(), extra_sheets.begin(), extra_sheets.end());
    ArchiveSet a360{argv[3], ".x360.bin", {}};
    std::ifstream table(argv[2]);
    if (!table) { std::fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
    std::string line;
    std::getline(table, line);
    while (std::getline(table, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::vector<std::string> c;
      std::stringstream ss(line);
      for (std::string cell; std::getline(ss, cell, ',');) c.push_back(cell);
      if (c.size() < 9 || std::find(wanted.begin(), wanted.end(), c[0]) == wanted.end()) continue;
      auto u = [&](int i) { return uint32_t(std::stoul(c[i])); };
      try {
        sheets.emplace_back(c[0], a360.read(c[1], u(2), u(3), u(4)), a360.read(c[5], u(6), u(7), u(8)));
      } catch (const std::exception& ex) {
        // The main font sheet is required; the others are skipped with a warning.
        std::fprintf(stderr, "%s: %s: %s\n", c[0] == "gui/resident/system_jp" ? "error" : "warning",
                     c[0].c_str(), ex.what());
        if (c[0] == "gui/resident/system_jp") return 3;
      }
    }
  }

  size_t written = 0;
  for (auto& [sheet, xgr, imgb] : sheets) {
    for (const Tex& t : ParseXgr(xgr)) {
      if (std::find(textures.begin(), textures.end(), t.name) == textures.end()) continue;
      FormatInfo fi;
      if (!GetFormat(t.format, fi) || t.depth > 1) {
        std::printf("  %s/%s: format %u not supported, skipped\n", sheet.c_str(), t.name.c_str(), t.format);
        continue;
      }
      std::vector<uint8_t> linear, raw, rgba;
      if (t.offset >= imgb.size() || !Untile360(&imgb[t.offset], imgb.size() - t.offset, t, fi, linear, raw)) {
        std::printf("  %s/%s: data missing, skipped\n", sheet.c_str(), t.name.c_str());
        continue;
      }
      bool empty = std::all_of(raw.begin(), raw.end(), [](uint8_t b) { return b == 0; });
      if (empty) {
        std::printf("  %s/%s: data is all zero (incomplete copy?), skipped\n", sheet.c_str(), t.name.c_str());
        continue;
      }
      if (fi.block == 4) {
        DecodeDxt(linear, t.width, t.height, t.format, rgba);
      } else {
        rgba.resize(size_t(t.width) * t.height * 4);
        for (size_t i = 0; i < size_t(t.width) * t.height; ++i)
          for (int k = 0; k < 4; ++k) rgba[i * 4 + k] = linear[i * 4 + (k + 1) % 4];  // A R G B -> R G B A
      }
      uint64_t hash = XXH3_64bits(raw.data(), raw.size());
      if (!hash) hash = 1;
      // Per channel: constant, a copy of an earlier channel, or coverage to reconstruct.
      // Planes are shared between channels (grey fonts: R = G = B, alpha constant).
      Plane src[4];
      std::vector<Plane> up_planes, bicubic_planes;
      int plane_of[4];
      for (int k = 0; k < 4; ++k) {
        src[k] = Plane(int(t.width), int(t.height));
        for (size_t i = 0; i < src[k].v.size(); ++i) src[k].v[i] = rgba[i * 4 + k] / 255.f;
      }
      up_planes.reserve(4);
      bicubic_planes.reserve(4);
      for (int k = 0; k < 4; ++k) {
        plane_of[k] = -1;
        for (int j = 0; j < k && plane_of[k] < 0; ++j)
          if (src[j].v == src[k].v) plane_of[k] = plane_of[j];
        if (plane_of[k] >= 0) continue;
        plane_of[k] = int(up_planes.size());
        float first = src[k].v[0];
        if (std::all_of(src[k].v.begin(), src[k].v.end(), [&](float v) { return v == first; })) {
          up_planes.emplace_back(1, 1, first);
          if (opt.preview) bicubic_planes.emplace_back(1, 1, first);
        } else {
          up_planes.push_back(UpscaleCoverage(src[k], opt));
          if (opt.preview) bicubic_planes.push_back(UpscaleBicubic(src[k], opt.scale));
        }
      }
      const Plane* up[4];
      const Plane* bicubic[4];
      for (int k = 0; k < 4; ++k) {
        up[k] = &up_planes[plane_of[k]];
        bicubic[k] = opt.preview ? &bicubic_planes[plane_of[k]] : nullptr;
      }
      const int out_w = int(t.width) * opt.scale, out_h = int(t.height) * opt.scale;
      std::vector<std::vector<uint8_t>> levels;
      {
        std::vector<Plane> level = up_planes;
        const Plane* lp[4];
        for (int k = 0; k < 4; ++k) lp[k] = &level[plane_of[k]];
        for (int w = out_w, h = out_h;;) {
          levels.push_back(EncodeLevel(lp, w, h, fi.dxgi));
          if (!opt.mips || w < 8 || h < 8) break;
          w /= 2;
          h /= 2;
          for (Plane& p : level) p = Downsample(p);
        }
      }
      char prefix[64];
      std::snprintf(prefix, sizeof(prefix), "%016llX_%ux%u_", (unsigned long long)hash, t.width, t.height);
      std::string file = std::string(prefix) + sheet + "_" + t.name + ".dds";
      std::replace(file.begin(), file.end(), '/', '-');
      if (!WriteDdsLevels(out_dir / file, uint32_t(out_w), uint32_t(out_h), fi.dxgi, levels)) {
        std::fprintf(stderr, "error: cannot write %s\n", (out_dir / file).string().c_str());
        return 3;
      }
      WriteSignature(out_dir / file, rgba, t.width, t.height);
      if (fi.block == 4 && fi.bytes == 8) {
        // The original atlas (untiled, guest byte order) for the renderer: the game adds glyphs
        // to the free space of the atlases at runtime, and the renderer rebuilds those blocks.
        fs::path src_path = out_dir / file;
        src_path.replace_extension(".src");
        std::ofstream src(src_path, std::ios::binary);
        uint32_t header[5] = {0, 1, t.width, t.height, fi.bytes};
        std::memcpy(header, "LRSR", 4);
        src.write(reinterpret_cast<const char*>(header), sizeof(header));
        src.write(reinterpret_cast<const char*>(raw.data()), std::streamsize(raw.size()));
        if (!src) {
          std::fprintf(stderr, "error: cannot write %s\n", src_path.string().c_str());
          return 3;
        }
      }
      ++written;
      std::printf("  %s/%s %ux%u -> %dx%d, %zu mips: %s\n", sheet.c_str(), t.name.c_str(), t.width, t.height,
                  out_w, out_h, levels.size(), file.c_str());
      char hash_text[20];
      std::snprintf(hash_text, sizeof(hash_text), "%016llX", (unsigned long long)hash);
      report << sheet << "," << t.name << "," << t.format << "," << t.width << "," << t.height << "," << out_w << ","
             << out_h << "," << levels.size() << "," << hash_text << "," << file << "\n";
      if (opt.preview) {
        std::string stem = sheet + "_" + t.name;
        std::replace(stem.begin(), stem.end(), '/', '-');
        const uint8_t* wp;
        uint32_t wsize;
        GlyphTable gt;
        if (FindWpdEntry(xgr, t.name + "t", "wfl", wp, wsize) && ParseWfl(wp, wsize, gt)) {
          const Plane* atlases[3] = {&src[0], bicubic[0], up[0]};
          int scales[3] = {1, opt.scale, opt.scale};
          PreviewText(out_dir / (stem + "_preview.png"), gt, atlases, scales, 3, 2);
        } else {  // top-left corner of the sheet: original (nearest), bicubic, reconstruction
          int cw = std::min(int(t.width), 128), chh = std::min(int(t.height), 64), s = opt.scale;
          int W = cw * s, H = chh * s * 3;
          std::vector<uint8_t> img(size_t(W) * H * 3);
          for (int r = 0; r < 3; ++r)
            for (int y = 0; y < chh * s; ++y)
              for (int x = 0; x < W; ++x)
                for (int k = 0; k < 3; ++k) {
                  float c = r == 0 ? src[k].at(x / s, y / s) : (r == 1 ? bicubic[k] : up[k])->clamped(x, y);
                  float a = r == 0 ? src[3].at(x / s, y / s) : (r == 1 ? bicubic[3] : up[3])->clamped(x, y);
                  float bg = ((x / 16 + y / 16) & 1) ? 0.45f : 0.3f;  // checkerboard behind alpha
                  img[(size_t(r * chh * s + y) * W + x) * 3 + k] =
                      uint8_t(std::lround(std::clamp(c * a + bg * (1 - a), 0.f, 1.f) * 255.f));
                }
          WritePng(out_dir / (stem + "_preview.png"), W, H, 3, img);
        }
      }
    }
  }
  std::printf("%zu font texture(s) written to %s\n", written, out_dir.string().c_str());
  return written ? 0 : 2;
}

}  // namespace fontup

int main(int argc, char** argv) {
  if (argc >= 2 && (std::strcmp(argv[1], "--fonts") == 0 || std::strcmp(argv[1], "--fonts-files") == 0)) {
    try {
      return fontup::FontsMain(argc, argv);
    } catch (const std::exception& ex) {
      std::fprintf(stderr, "error: %s\n", ex.what());
      return 3;
    }
  }
  // lr_uiconv <360 folder> <PS3 folder> <output folder>          (extracted .xgr/.imgb files)
  // lr_uiconv --table <ui_sheets.csv> <360 sys folder> <PS3 sys folder> <output folder>
  //   reads the sheets straight out of the white_img*.bin archives listed in the table
  bool table_mode = argc >= 6 && std::strcmp(argv[1], "--table") == 0;
  if (argc < 4 || (std::strcmp(argv[1], "--table") == 0 && !table_mode)) {
    std::fprintf(stderr, "usage: lr_uiconv <360 folder> <PS3 folder> <output folder>\n"
                         "       lr_uiconv --table <ui_sheets.csv> <360 sys folder> <PS3 sys folder> <output folder>\n"
                         "       lr_uiconv --fonts <ui_sheets.csv> <360 sys folder> <output folder> [options]\n");
    return 1;
  }
  fs::path out_dir = table_mode ? argv[5] : argv[3];
  fs::create_directories(out_dir);
  std::ofstream report(out_dir / "report.csv");
  report << "sheet,texture,format,width,height,difference,result,hash360\n";
  size_t compared = 0, same = 0, written = 0, skipped = 0;
  auto process = [&](const std::string& sheet, const std::vector<uint8_t>& xgr_a, const std::vector<uint8_t>& data_a,
                     const std::vector<uint8_t>& xgr_b, const std::vector<uint8_t>& data_b) {
    auto texs_a = ParseXgr(xgr_a), texs_b = ParseXgr(xgr_b);
    for (const Tex& a : texs_a) {
      auto it = std::find_if(texs_b.begin(), texs_b.end(), [&](const Tex& b) { return b.name == a.name; });
      FormatInfo fi;
      if (it == texs_b.end() || !GetFormat(a.format, fi) || a.depth > 1 || it->format != a.format ||
          it->width != a.width || it->height != a.height) {
        ++skipped;
        continue;
      }
      // Only base levels the renderer can hash (no packed mip tail inside the base).
      if (a.mips > 1 && (a.width < 64 || a.height < 64)) { ++skipped; continue; }
      uint32_t bw = (a.width + fi.block - 1) / fi.block, bh = (a.height + fi.block - 1) / fi.block;
      size_t base_size = size_t((bw + 31) & ~31u) * ((bh + 31) & ~31u) * fi.bytes;
      if (uint64_t(a.offset) + base_size > data_a.size() || it->offset >= data_b.size()) { ++skipped; continue; }
      std::vector<uint8_t> linear, raw, ref;
      if (!Untile360(&data_a[a.offset], data_a.size() - a.offset, a, fi, linear, raw)) { ++skipped; continue; }
      if (fi.block == 4) {
        DecodeDxt(linear, a.width, a.height, a.format, ref);
      } else {
        ref.resize(size_t(a.width) * a.height * 4);
        for (size_t i = 0; i < size_t(a.width) * a.height; ++i) {  // A R G B -> R G B A
          ref[i * 4 + 0] = linear[i * 4 + 1];
          ref[i * 4 + 1] = linear[i * 4 + 2];
          ref[i * 4 + 2] = linear[i * 4 + 3];
          ref[i * 4 + 3] = linear[i * 4 + 0];
        }
      }
      Candidate c = DecodeOther(&data_b[it->offset], data_b.size() - it->offset, *it, fi, ref);
      ++compared;
      uint64_t hash = XXH3_64bits(raw.data(), raw.size());
      if (!hash) hash = 1;
      const char* result = "same";
      if (c.diff > 1.0) {
        result = "unreadable";
      } else if (c.diff > 0.003) {
        char name[64];
        std::snprintf(name, sizeof(name), "%016llX_%ux%u_", (unsigned long long)hash, a.width, a.height);
        std::string file = std::string(name) + sheet + "_" + a.name + ".dds";
        std::replace(file.begin(), file.end(), '/', '-');
        const std::vector<uint8_t>& payload = fi.block == 4 ? c.blocks : c.rgba;
        result = WriteDds(out_dir / file, a.width, a.height, fi.dxgi, payload) ? "replaced" : "write failed";
        WriteSignature(out_dir / file, ref, a.width, a.height);
        if (std::strcmp(result, "replaced") == 0) ++written;
      } else {
        ++same;
      }
      char hash_text[20];
      std::snprintf(hash_text, sizeof(hash_text), "%016llX", (unsigned long long)hash);
      report << sheet << "," << a.name << "," << a.format << "," << a.width << "," << a.height << "," << c.diff << ","
             << result << "," << hash_text << "\n";
    }
  };
  if (table_mode) {
    ArchiveSet a360{argv[3], ".x360.bin", {}}, aps3{argv[4], ".ps3.bin", {}};
    std::ifstream table(argv[2]);
    std::string line;
    std::getline(table, line);  // header
    size_t rows = 0;
    while (std::getline(table, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::vector<std::string> c;
      std::stringstream ss(line);
      for (std::string cell; std::getline(ss, cell, ',');) c.push_back(cell);
      if (c.size() < 17) continue;
      auto u = [&](int i) { return uint32_t(std::stoul(c[i])); };
      try {
        Bytes xa = a360.read(c[1], u(2), u(3), u(4)), da = a360.read(c[5], u(6), u(7), u(8));
        Bytes xb = aps3.read(c[9], u(10), u(11), u(12)), db = aps3.read(c[13], u(14), u(15), u(16));
        process(c[0], xa, da, xb, db);
      } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s: %s\n", c[0].c_str(), ex.what());
        return 3;
      }
      if (++rows % 100 == 0) std::printf("  %zu sheets\n", rows);
    }
  } else {
    fs::path dir360 = argv[1], dir_other = argv[2];
    for (auto& entry : fs::recursive_directory_iterator(dir360)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".xgr") continue;
      fs::path rel = fs::relative(entry.path(), dir360);
      fs::path other_xgr = dir_other / rel;
      fs::path imgb360 = entry.path(), imgb_other = other_xgr;
      imgb360.replace_extension(".imgb");
      imgb_other.replace_extension(".imgb");
      if (!fs::exists(other_xgr) || !fs::exists(imgb360) || !fs::exists(imgb_other)) continue;
      std::string sheet = rel.generic_string();
      sheet = sheet.substr(0, sheet.size() - 4);
      process(sheet, ReadFile(entry.path()), ReadFile(imgb360), ReadFile(other_xgr), ReadFile(imgb_other));
    }
  }
  std::printf("compared %zu textures: %zu the same, %zu different (written), %zu skipped\n", compared, same, written,
              skipped);
  // Most UI art is identical on both consoles; if almost nothing matched, the PS3 data was
  // not decoded correctly and the output would be garbage.
  if (compared && same * 2 < compared) {
    std::printf("WARNING: fewer than half the textures matched - decoding problem, output not usable\n");
    return 2;
  }
  return 0;
}
