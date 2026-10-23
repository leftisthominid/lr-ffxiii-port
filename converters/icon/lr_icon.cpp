// lr_icon - the port's icon (Windows exe and Android app), made from the player's own game at
// build time: the Xbox 360 title tile (Lightning's portrait) in default.xex.
//
//   lr_icon extract <default.xex> <tile.png>
//       The 64x64 title image: the xex's PE image (AES-128, retail key; basic compression), its
//       title resource (XDBF), image entry 0x8000.
//   lr_icon title <360 game folder> <face.png>
//       Lightning's face from the title screen's background (texture titlew01 of title0.x360.xgr,
//       in weiss_data/sys/white_img2u.x360.bin: the band behind the title menu), a 168x168 square.
//   lr_icon compose <face.png> <out dir>
//       face.png: a square portrait (the face, ideally upscaled 4x with Real-ESRGAN). Writes
//       lr_icon_full.png (432x432: the portrait over a blurred copy of itself - the Android
//       adaptive icon background), ic_launcher_<dpi>.png (the portrait as rounded squares,
//       48-192 px), and lightningreturns.ico (16-256 px).
//
// The xex and the tile are only read; nothing from the game is kept in the source.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

static std::vector<uint8_t> ReadFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

// ---- AES-128 (decryption only needs the inverse cipher plus the key schedule) ----
struct Aes {
  uint8_t rk[176], sbox[256], inv[256];
  static uint8_t Xt(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
  static uint8_t Mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (; b; b >>= 1, a = Xt(a)) if (b & 1) p ^= a;
    return p;
  }
  explicit Aes(const uint8_t key[16]) {
    for (int x = 0; x < 256; ++x) {
      uint8_t v = 0;
      for (int y = 1; y < 256 && x; ++y) if (Mul(uint8_t(x), uint8_t(y)) == 1) v = uint8_t(y);
      uint8_t s = v;
      for (int i = 1; i < 5; ++i) s ^= uint8_t((v << i) | (v >> (8 - i)));
      sbox[x] = s ^ 0x63;
      inv[sbox[x]] = uint8_t(x);
    }
    std::memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
      if (i % 16 == 0) {
        uint8_t f = t[0];
        t[0] = uint8_t(sbox[t[1]] ^ rcon); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[f];
        rcon = Xt(rcon);
      }
      for (int j = 0; j < 4; ++j) rk[i + j] = uint8_t(rk[i - 16 + j] ^ t[j]);
    }
  }
  void Decrypt(uint8_t b[16]) const {
    for (int i = 0; i < 16; ++i) b[i] ^= rk[160 + i];
    for (int r = 9; r >= 0; --r) {
      uint8_t t[16];
      for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) t[c * 4 + row] = b[((c - row + 4) % 4) * 4 + row];
      for (int i = 0; i < 16; ++i) b[i] = inv[t[i]] ^ rk[r * 16 + i];
      if (r)
        for (int c = 0; c < 4; ++c) {
          uint8_t* col = b + c * 4;
          uint8_t m[4];
          for (int row = 0; row < 4; ++row)
            m[row] = Mul(col[row], 14) ^ Mul(col[(row + 1) % 4], 11) ^ Mul(col[(row + 2) % 4], 13) ^ Mul(col[(row + 3) % 4], 9);
          std::memcpy(col, m, 4);
        }
    }
  }
};

static int Extract(const fs::path& xex_path, const fs::path& out) {
  std::vector<uint8_t> x = ReadFile(xex_path);
  if (x.size() < 0x200 || std::memcmp(x.data(), "XEX2", 4) != 0) { std::fprintf(stderr, "not an XEX2 file\n"); return 2; }
  const uint32_t pe_off = BE32(&x[8]), sec_off = BE32(&x[0x10]), count = BE32(&x[0x14]);
  uint32_t ffi = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (BE32(&x[0x18 + i * 8]) == 0x3FF) ffi = BE32(&x[0x18 + i * 8 + 4]);
  if (!ffi) { std::fprintf(stderr, "no file format header\n"); return 2; }
  const uint32_t info_size = BE32(&x[ffi]);
  const uint16_t encryption = BE16(&x[ffi + 4]), compression = BE16(&x[ffi + 6]);
  if (compression != 1) { std::fprintf(stderr, "compression %u not supported (only basic)\n", compression); return 2; }
  // The session key: the file key decrypted with the retail key; the data is AES-CBC (IV 0).
  static const uint8_t kRetail[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                      0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
  uint8_t key[16];
  std::memcpy(key, &x[sec_off + 0x150], 16);
  Aes(kRetail).Decrypt(key);
  std::vector<uint8_t> data(x.begin() + pe_off, x.end());
  data.resize(data.size() & ~size_t(15));
  if (encryption == 1) {
    const Aes aes(key);
    uint8_t iv[16] = {}, prev[16];
    for (size_t o = 0; o < data.size(); o += 16) {
      std::memcpy(prev, &data[o], 16);
      aes.Decrypt(&data[o]);
      for (int i = 0; i < 16; ++i) data[o + i] ^= iv[i];
      std::memcpy(iv, prev, 16);
    }
  }
  std::vector<uint8_t> image;
  size_t p = 0;
  for (uint32_t b = 0; b + 8 <= info_size - 8; b += 8) {
    const uint32_t data_size = BE32(&x[ffi + 8 + b]), zero_size = BE32(&x[ffi + 12 + b]);
    if (p + data_size > data.size()) break;
    image.insert(image.end(), data.begin() + p, data.begin() + p + data_size);
    image.insert(image.end(), zero_size, 0);
    p += data_size;
  }
  // The title resource (XDBF): its image entry 0x8000 is the title image (PNG).
  for (size_t o = 0; o + 24 <= image.size(); ++o) {
    if (std::memcmp(&image[o], "XDBF", 4) != 0) continue;
    const uint8_t* h = &image[o];
    const uint32_t entry_max = BE32(h + 8), entries = BE32(h + 12), free_max = BE32(h + 16);
    const size_t base = o + 24 + size_t(entry_max) * 18 + size_t(free_max) * 8;
    for (uint32_t e = 0; e < entries && o + 24 + (e + 1) * 18 <= image.size(); ++e) {
      const uint8_t* en = h + 24 + e * 18;
      const uint16_t ns = BE16(en);
      const uint64_t id = uint64_t(BE32(en + 2)) << 32 | BE32(en + 6);
      const uint32_t off = BE32(en + 10), len = BE32(en + 14);
      if (ns == 2 && id == 0x8000 && base + off + len <= image.size() && std::memcmp(&image[base + off], "\x89PNG", 4) == 0) {
        // Written as RGB (the upscaler's input).
        int w, h, n;
        uint8_t* rgb = stbi_load_from_memory(&image[base + off], int(len), &w, &h, &n, 3);
        if (!rgb) { std::fprintf(stderr, "the title image is not a readable PNG\n"); return 4; }
        const bool ok = stbi_write_png(out.string().c_str(), w, h, 3, rgb, w * 3) != 0;
        stbi_image_free(rgb);
        std::printf("title image: %dx%d -> %s\n", w, h, out.string().c_str());
        return ok ? 0 : 3;
      }
    }
  }
  std::fprintf(stderr, "no title image in the xex\n");
  return 4;
}

// The title screen background: white_img2u.x360.bin of the 360 release, title0.x360.imgb (zlib)
// at sector 0x16F37; titlew01 is 1280x1024 linear ARGB at 3350528 in it; the menu band with
// Lightning's face is rows 168-335, the face around x 935.
static int Title(const fs::path& game, const fs::path& out) {
  const fs::path pack = game / "weiss_data" / "sys" / "white_img2u.x360.bin";
  std::ifstream in(pack, std::ios::binary);
  in.seekg(0, std::ios::end);
  if (!in || uint64_t(in.tellg()) != 583780352ull) { std::fprintf(stderr, "%s is not the 360 release's file\n", pack.string().c_str()); return 2; }
  std::vector<char> z(0x36B8FF);
  in.seekg(std::streamoff(0x16F37) * 2048);
  in.read(z.data(), std::streamsize(z.size()));
  int len = 0;
  char* imgb = stbi_zlib_decode_malloc(z.data(), int(z.size()), &len);
  if (!imgb || len != 0x832000) { std::fprintf(stderr, "title0.x360.imgb did not unpack\n"); free(imgb); return 3; }
  const int kW = 1280, kSize = 168, kX = 935 - kSize / 2, kY = 168;
  std::vector<uint8_t> rgb(size_t(kSize) * kSize * 3);
  for (int y = 0; y < kSize; ++y)
    for (int x = 0; x < kSize; ++x) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(imgb) + 3350528 + (size_t(kY + y) * kW + kX + x) * 4;  // A R G B
      uint8_t* q = &rgb[(size_t(y) * kSize + x) * 3];
      q[0] = p[1]; q[1] = p[2]; q[2] = p[3];
    }
  free(imgb);
  const bool ok = stbi_write_png(out.string().c_str(), kSize, kSize, 3, rgb.data(), kSize * 3) != 0;
  std::printf("face: %dx%d -> %s\n", kSize, kSize, out.string().c_str());
  return ok ? 0 : 3;
}

// ---- image helpers (RGB float) ----
struct Img {
  int w = 0, h = 0;
  std::vector<float> px;  // RGB
  float* at(int x, int y) { return &px[(size_t(y) * w + x) * 3]; }
  const float* at(int x, int y) const { return &px[(size_t(y) * w + x) * 3]; }
};
static Img Make(int w, int h) { Img i; i.w = w; i.h = h; i.px.assign(size_t(w) * h * 3, 0.f); return i; }

static double Lanczos(double x) {
  x = std::fabs(x);
  if (x < 1e-9) return 1;
  if (x >= 3) return 0;
  const double pi = 3.14159265358979323846;
  return 3 * std::sin(pi * x) * std::sin(pi * x / 3) / (pi * pi * x * x);
}
// Separable Lanczos-3 resize (support widened when shrinking, as PIL does).
static Img Resize(const Img& s, int w, int h) {
  auto pass = [](const Img& in, int out_w, int out_h, bool horizontal) {
    Img o = Make(out_w, out_h);
    const int src_len = horizontal ? in.w : in.h, dst_len = horizontal ? out_w : out_h;
    const double scale = double(src_len) / dst_len, support = 3 * std::max(1.0, scale);
    for (int d = 0; d < dst_len; ++d) {
      const double c = (d + 0.5) * scale;
      const int lo = std::max(0, int(std::floor(c - support))), hi = std::min(src_len - 1, int(std::ceil(c + support)));
      std::vector<double> wts;
      double sum = 0;
      for (int k = lo; k <= hi; ++k) { double wv = Lanczos((k + 0.5 - c) / std::max(1.0, scale)); wts.push_back(wv); sum += wv; }
      const int other = horizontal ? out_h : out_w;
      for (int t = 0; t < other; ++t) {
        double acc[3] = {0, 0, 0};
        for (int k = lo; k <= hi; ++k) {
          const float* p = horizontal ? in.at(k, t) : in.at(t, k);
          const double wv = wts[k - lo] / sum;
          for (int ch = 0; ch < 3; ++ch) acc[ch] += p[ch] * wv;
        }
        float* q = horizontal ? o.at(d, t) : o.at(t, d);
        for (int ch = 0; ch < 3; ++ch) q[ch] = float(std::clamp(acc[ch], 0.0, 255.0));
      }
    }
    return o;
  };
  return pass(pass(s, w, s.h, true), w, h, false);
}
static std::vector<float> Kernel(double sigma) {
  const int r = std::max(1, int(std::ceil(sigma * 3)));
  std::vector<float> k(2 * r + 1);
  double sum = 0;
  for (int i = -r; i <= r; ++i) sum += k[i + r] = float(std::exp(-i * i / (2 * sigma * sigma)));
  for (float& v : k) v = float(v / sum);
  return k;
}
static void BlurPlane(std::vector<float>& v, int w, int h, int channels, double sigma) {
  const std::vector<float> k = Kernel(sigma);
  const int r = int(k.size() / 2);
  std::vector<float> t(v.size());
  for (int pass = 0; pass < 2; ++pass) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        for (int c = 0; c < channels; ++c) {
          double acc = 0;
          for (int i = -r; i <= r; ++i) {
            int xx = pass == 0 ? std::clamp(x + i, 0, w - 1) : x, yy = pass == 1 ? std::clamp(y + i, 0, h - 1) : y;
            acc += v[(size_t(yy) * w + xx) * channels + c] * k[i + r];
          }
          t[(size_t(y) * w + x) * channels + c] = float(acc);
        }
    v.swap(t);
  }
}
static Img Crop(const Img& s, int x0, int y0, int x1, int y1) {
  Img o = Make(x1 - x0, y1 - y0);
  for (int y = y0; y < y1; ++y) std::memcpy(o.at(0, y - y0), s.at(x0, y), sizeof(float) * 3 * o.w);
  return o;
}
// Rounded-rectangle coverage (1 inside, 0 outside) for a w x h box with corner radius r.
static float RoundedInside(int x, int y, int w, int h, float r) {
  const float cx = std::clamp(float(x) + 0.5f, r, float(w) - r), cy = std::clamp(float(y) + 0.5f, r, float(h) - r);
  const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy;
  return dx * dx + dy * dy <= r * r ? 1.f : 0.f;
}
static std::vector<uint8_t> ToRgba(const Img& im, float radius) {
  std::vector<uint8_t> o(size_t(im.w) * im.h * 4);
  for (int y = 0; y < im.h; ++y)
    for (int x = 0; x < im.w; ++x) {
      const float* p = im.at(x, y);
      uint8_t* q = &o[(size_t(y) * im.w + x) * 4];
      const bool in = radius <= 0 || RoundedInside(x, y, im.w, im.h, radius) > 0;
      for (int c = 0; c < 3; ++c) q[c] = in ? uint8_t(std::lround(p[c])) : 0;
      q[3] = in ? 255 : 0;
    }
  return o;
}
static void WriteMem(void* ctx, void* data, int size) {
  auto* v = static_cast<std::vector<uint8_t>*>(ctx);
  v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}
static std::vector<uint8_t> Png(const std::vector<uint8_t>& rgba, int w, int h, int comp) {
  std::vector<uint8_t> out;
  stbi_write_png_to_func(WriteMem, &out, w, h, comp, rgba.data(), w * comp);
  return out;
}
static bool Save(const fs::path& p, const std::vector<uint8_t>& bytes) {
  std::ofstream o(p, std::ios::binary);
  o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  return o.good();
}

static int Compose(const fs::path& tile_path, const fs::path& dir) {
  int w, h, n;
  uint8_t* data = stbi_load(tile_path.string().c_str(), &w, &h, &n, 3);
  if (!data) { std::fprintf(stderr, "cannot read %s\n", tile_path.string().c_str()); return 2; }
  Img tile = Make(w, h);
  for (size_t i = 0; i < tile.px.size(); ++i) tile.px[i] = data[i];
  stbi_image_free(data);
  const Img im = tile.w == tile.h ? tile : Crop(tile, 0, 0, std::min(tile.w, tile.h), std::min(tile.w, tile.h));
  // The portrait (slightly sharpened) on a 432 canvas, over a blurred copy, feathered edge.
  const int kCanvas = 432, kPortrait = 316;
  Img por = Resize(im, kPortrait, kPortrait);
  {
    std::vector<float> blurred = por.px;
    BlurPlane(blurred, por.w, por.h, 3, 1.0);
    for (size_t i = 0; i < por.px.size(); ++i) {
      const float d = por.px[i] - blurred[i];
      if (std::fabs(d) >= 2.f) por.px[i] = std::clamp(por.px[i] + d * 0.35f, 0.f, 255.f);
    }
  }
  Img canvas = Resize(im, kCanvas, kCanvas);
  BlurPlane(canvas.px, kCanvas, kCanvas, 3, 18.0);
  std::vector<float> mask(size_t(kPortrait) * kPortrait);
  for (int y = 0; y < kPortrait; ++y)
    for (int x = 0; x < kPortrait; ++x) mask[size_t(y) * kPortrait + x] = RoundedInside(x, y, kPortrait, kPortrait, 40.f);
  BlurPlane(mask, kPortrait, kPortrait, 1, 6.0);
  const int off = (kCanvas - kPortrait) / 2;
  for (int y = 0; y < kPortrait; ++y)
    for (int x = 0; x < kPortrait; ++x) {
      const float a = mask[size_t(y) * kPortrait + x];
      float* c = canvas.at(x + off, y + off);
      const float* p = por.at(x, y);
      for (int ch = 0; ch < 3; ++ch) c[ch] = c[ch] * (1 - a) + p[ch] * a;
    }
  fs::create_directories(dir);
  bool ok = Save(dir / "lr_icon_full.png", Png(ToRgba(canvas, 0), kCanvas, kCanvas, 4));
  // Older launchers and Windows: the portrait itself, as rounded squares.
  const Img& visible = im;
  const struct { const char* name; int px; } dpis[] = {{"mdpi", 48}, {"hdpi", 72}, {"xhdpi", 96}, {"xxhdpi", 144}, {"xxxhdpi", 192}};
  for (const auto& d : dpis) {
    const Img s = Resize(visible, d.px, d.px);
    ok &= Save(dir / (std::string("ic_launcher_") + d.name + ".png"), Png(ToRgba(s, float(d.px / 5)), d.px, d.px, 4));
  }
  // Windows icon: 256 px as PNG, the smaller sizes as 32-bit bitmaps (what every resource
  // compiler and Windows version reads).
  const int sizes[] = {256, 128, 64, 48, 32, 24, 16};
  std::vector<std::vector<uint8_t>> pngs;
  for (int s : sizes) {
    const std::vector<uint8_t> rgba = ToRgba(Resize(visible, s, s), float(s / 5));
    if (s == 256) { pngs.push_back(Png(rgba, s, s, 4)); continue; }
    std::vector<uint8_t> dib(40, 0);
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) dib[at + i] = uint8_t(v >> (8 * i)); };
    const uint32_t mask_row = ((uint32_t(s) + 31) / 32) * 4;
    put32(0, 40); put32(4, uint32_t(s)); put32(8, uint32_t(s) * 2);  // height: image + AND mask
    dib[12] = 1; dib[14] = 32;                                        // planes, bits per pixel
    put32(20, uint32_t(s) * s * 4 + mask_row * s);
    for (int y = s - 1; y >= 0; --y)  // bottom-up BGRA
      for (int x = 0; x < s; ++x) {
        const uint8_t* q = &rgba[(size_t(y) * s + x) * 4];
        dib.insert(dib.end(), {q[2], q[1], q[0], q[3]});
      }
    for (int y = s - 1; y >= 0; --y) {  // AND mask: 1 = transparent
      std::vector<uint8_t> row(mask_row, 0);
      for (int x = 0; x < s; ++x)
        if (rgba[(size_t(y) * s + x) * 4 + 3] == 0) row[x / 8] |= uint8_t(0x80 >> (x % 8));
      dib.insert(dib.end(), row.begin(), row.end());
    }
    pngs.push_back(dib);
  }
  std::vector<uint8_t> ico = {0, 0, 1, 0, uint8_t(std::size(sizes)), 0};
  uint32_t offset = 6 + 16 * uint32_t(std::size(sizes));
  for (size_t i = 0; i < std::size(sizes); ++i) {
    const int s = sizes[i];
    const uint32_t len = uint32_t(pngs[i].size());
    const uint8_t e[16] = {uint8_t(s == 256 ? 0 : s), uint8_t(s == 256 ? 0 : s), 0, 0, 1, 0, 32, 0,
                           uint8_t(len), uint8_t(len >> 8), uint8_t(len >> 16), uint8_t(len >> 24),
                           uint8_t(offset), uint8_t(offset >> 8), uint8_t(offset >> 16), uint8_t(offset >> 24)};
    ico.insert(ico.end(), e, e + 16);
    offset += len;
  }
  for (const auto& p : pngs) ico.insert(ico.end(), p.begin(), p.end());
  ok &= Save(dir / "lightningreturns.ico", ico);
  std::printf("icon written to %s\n", dir.string().c_str());
  return ok ? 0 : 3;
}

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "extract") return Extract(fs::u8path(argv[2]), fs::u8path(argv[3]));
  if (argc == 4 && std::string(argv[1]) == "title") return Title(fs::u8path(argv[2]), fs::u8path(argv[3]));
  if (argc == 4 && std::string(argv[1]) == "compose") return Compose(fs::u8path(argv[2]), fs::u8path(argv[3]));
  std::fprintf(stderr, "usage: lr_icon title <360 game folder> <face.png> | extract <default.xex> <tile.png> | compose <face.png> <out dir>\n");
  return 1;
}
