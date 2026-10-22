/**
 * @file        graphics/pipeline/texture/replacement.cpp
 * @brief       Hash-keyed texture replacement and dumping (Lightning Returns port)
 */

#include <rex/graphics/pipeline/texture/replacement.h>

#include <cstdio>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

#include <rex/logging.h>
#include <xxhash.h>

namespace rex::graphics::texture_replacement {

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
         (uint32_t(uint8_t(d)) << 24);
}

#pragma pack(push, 1)
struct DdsPixelFormat {
  uint32_t size, flags, four_cc, rgb_bit_count, r_mask, g_mask, b_mask, a_mask;
};
struct DdsHeader {
  uint32_t size, flags, height, width, pitch_or_linear_size, depth, mip_map_count;
  uint32_t reserved1[11];
  DdsPixelFormat pf;
  uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct DdsHeaderDx10 {
  uint32_t dxgi_format, resource_dimension, misc_flag, array_size, misc_flags2;
};
#pragma pack(pop)
static_assert(sizeof(DdsHeader) == 124);

constexpr uint32_t kDdpfAlphaPixels = 0x1, kDdpfFourCC = 0x4, kDdpfRgb = 0x40;
constexpr uint32_t kDdsdCaps = 0x1, kDdsdHeight = 0x2, kDdsdWidth = 0x4, kDdsdPixelFormat = 0x1000,
                   kDdsdMipMapCount = 0x20000, kDdsdLinearSize = 0x80000;
constexpr uint32_t kCapsTexture = 0x1000, kCapsMipMap = 0x400000, kCapsComplex = 0x8;
constexpr uint32_t kCaps2Cubemap = 0x200, kCaps2Volume = 0x200000;

uint32_t NormalizeDxgi(uint32_t f) {
  switch (f) {
    case 27: case 28: case 29: return kDxgiR8G8B8A8Unorm;
    case 70: case 71: case 72: return kDxgiBC1Unorm;
    case 73: case 74: case 75: return kDxgiBC2Unorm;
    case 76: case 77: case 78: return kDxgiBC3Unorm;
    case 79: case 80: return kDxgiBC4Unorm;
    case 82: case 83: return kDxgiBC5Unorm;
    case 87: case 90: case 91: return kDxgiB8G8R8A8Unorm;
    case 97: case 98: case 99: return kDxgiBC7Unorm;
    default: return 0;
  }
}

uint64_t ParseHashPrefix(const std::string& stem, bool& ok) {
  ok = false;
  if (stem.size() < 16) return 0;
  uint64_t v = 0;
  for (size_t i = 0; i < 16; ++i) {
    char c = stem[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = uint32_t(c - '0');
    else if (c >= 'a' && c <= 'f') d = uint32_t(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = uint32_t(c - 'A' + 10);
    else return 0;
    v = (v << 4) | d;
  }
  if (stem.size() > 16 && std::isxdigit(static_cast<unsigned char>(stem[16]))) return 0;
  ok = true;
  return v;
}

}  // namespace

uint32_t BytesPerBlock(uint32_t f) {
  switch (f) {
    case kDxgiBC1Unorm: case kDxgiBC4Unorm: return 8;
    case kDxgiBC2Unorm: case kDxgiBC3Unorm: case kDxgiBC5Unorm: case kDxgiBC7Unorm: return 16;
    case kDxgiR8G8B8A8Unorm: case kDxgiB8G8R8A8Unorm: return 4;
    default: return 0;
  }
}

uint32_t BlockSize(uint32_t f) {
  switch (f) {
    case kDxgiR8G8B8A8Unorm: case kDxgiB8G8R8A8Unorm: return 1;
    case 0: return 0;
    default: return BytesPerBlock(f) ? 4 : 0;
  }
}

bool Image::IsBlockCompressed() const { return BlockSize(dxgi_format) == 4; }

bool LoadDds(const std::filesystem::path& path, Image& image, std::string& error) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    error = "cannot open";
    return false;
  }
  std::streamsize file_size = file.tellg();
  file.seekg(0);
  uint32_t magic = 0;
  DdsHeader header{};
  if (file_size < std::streamsize(4 + sizeof(header)) || !file.read(reinterpret_cast<char*>(&magic), 4) ||
      magic != FourCC('D', 'D', 'S', ' ') ||
      !file.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.size != 124) {
    error = "not a DDS file";
    return false;
  }
  if ((header.caps2 & (kCaps2Cubemap | kCaps2Volume)) || header.depth > 1) {
    error = "cube maps and volume textures are not supported";
    return false;
  }
  uint32_t format = 0;
  if (header.pf.flags & kDdpfFourCC) {
    switch (header.pf.four_cc) {
      case FourCC('D', 'X', 'T', '1'): format = kDxgiBC1Unorm; break;
      case FourCC('D', 'X', 'T', '2'): case FourCC('D', 'X', 'T', '3'): format = kDxgiBC2Unorm; break;
      case FourCC('D', 'X', 'T', '4'): case FourCC('D', 'X', 'T', '5'): format = kDxgiBC3Unorm; break;
      case FourCC('A', 'T', 'I', '1'): case FourCC('B', 'C', '4', 'U'): format = kDxgiBC4Unorm; break;
      case FourCC('A', 'T', 'I', '2'): case FourCC('B', 'C', '5', 'U'): format = kDxgiBC5Unorm; break;
      case FourCC('D', 'X', '1', '0'): {
        DdsHeaderDx10 dx10{};
        if (!file.read(reinterpret_cast<char*>(&dx10), sizeof(dx10))) {
          error = "truncated DX10 header";
          return false;
        }
        if (dx10.resource_dimension != 3 || dx10.array_size > 1 || (dx10.misc_flag & 0x4)) {
          error = "only single 2D textures are supported";
          return false;
        }
        format = NormalizeDxgi(dx10.dxgi_format);
        break;
      }
      default: break;
    }
  } else if ((header.pf.flags & kDdpfRgb) && header.pf.rgb_bit_count == 32) {
    if (header.pf.r_mask == 0x000000FF && header.pf.g_mask == 0x0000FF00 && header.pf.b_mask == 0x00FF0000) {
      format = kDxgiR8G8B8A8Unorm;
    } else if (header.pf.r_mask == 0x00FF0000 && header.pf.g_mask == 0x0000FF00 &&
               header.pf.b_mask == 0x000000FF) {
      format = kDxgiB8G8R8A8Unorm;
    }
  }
  if (!format) {
    error = "unsupported pixel format (use BC1/BC2/BC3/BC4/BC5/BC7 or 32-bit RGBA)";
    return false;
  }
  if (!header.width || !header.height || header.width > 16384 || header.height > 16384) {
    error = "bad size";
    return false;
  }
  uint32_t mip_count = std::max(header.mip_map_count, UINT32_C(1));
  uint32_t max_mips = 1;
  while ((std::max(header.width, header.height) >> max_mips) != 0) ++max_mips;
  mip_count = std::min(mip_count, max_mips);

  image.width = header.width;
  image.height = header.height;
  image.dxgi_format = format;
  image.levels.clear();
  uint32_t block = BlockSize(format), bpb = BytesPerBlock(format);
  size_t total = 0;
  for (uint32_t level = 0; level < mip_count; ++level) {
    uint32_t w = std::max(header.width >> level, UINT32_C(1));
    uint32_t h = std::max(header.height >> level, UINT32_C(1));
    Image::Level l;
    l.offset = total;
    l.row_pitch = ((w + block - 1) / block) * bpb;
    l.rows = (h + block - 1) / block;
    total += size_t(l.row_pitch) * l.rows;
    image.levels.push_back(l);
  }
  std::streamsize data_start = file.tellg();
  if (file_size - data_start < std::streamsize(image.levels[0].row_pitch) * image.levels[0].rows) {
    error = "truncated";
    return false;
  }
  // A file with fewer mips than its header claims keeps the complete levels.
  while (std::streamsize(total) > file_size - data_start) {
    const Image::Level& last = image.levels.back();
    total -= size_t(last.row_pitch) * last.rows;
    image.levels.pop_back();
  }
  image.data.resize(total);
  if (!file.read(reinterpret_cast<char*>(image.data.data()), std::streamsize(total))) {
    error = "read failed";
    return false;
  }
  return true;
}

bool SaveDds(const std::filesystem::path& path, uint32_t width, uint32_t height, uint32_t format,
             const void* data, size_t size) {
  DdsHeader header{};
  header.size = 124;
  header.flags = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat | kDdsdLinearSize;
  header.width = width;
  header.height = height;
  header.mip_map_count = 1;
  header.pitch_or_linear_size = uint32_t(size);
  header.caps = kCapsTexture;
  header.pf.size = 32;
  switch (format) {
    case kDxgiBC1Unorm: header.pf.flags = kDdpfFourCC; header.pf.four_cc = FourCC('D', 'X', 'T', '1'); break;
    case kDxgiBC2Unorm: header.pf.flags = kDdpfFourCC; header.pf.four_cc = FourCC('D', 'X', 'T', '3'); break;
    case kDxgiBC3Unorm: header.pf.flags = kDdpfFourCC; header.pf.four_cc = FourCC('D', 'X', 'T', '5'); break;
    case kDxgiBC4Unorm: header.pf.flags = kDdpfFourCC; header.pf.four_cc = FourCC('A', 'T', 'I', '1'); break;
    case kDxgiBC5Unorm: header.pf.flags = kDdpfFourCC; header.pf.four_cc = FourCC('A', 'T', 'I', '2'); break;
    case kDxgiR8G8B8A8Unorm:
      header.pf.flags = kDdpfRgb | kDdpfAlphaPixels;
      header.pf.rgb_bit_count = 32;
      header.pf.r_mask = 0x000000FF;
      header.pf.g_mask = 0x0000FF00;
      header.pf.b_mask = 0x00FF0000;
      header.pf.a_mask = 0xFF000000;
      break;
    default:
      return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary);
  if (!file) return false;
  uint32_t magic = FourCC('D', 'D', 'S', ' ');
  file.write(reinterpret_cast<const char*>(&magic), 4);
  file.write(reinterpret_cast<const char*>(&header), sizeof(header));
  file.write(reinterpret_cast<const char*>(data), std::streamsize(size));
  return bool(file);
}

namespace {
// Xenos 2D tiled address of block (x, y), in blocks; pitch in blocks, aligned to 32.
uint32_t TiledBlockOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bytes_per_block) {
  uint32_t log_bpp = (bytes_per_block >> 2) + ((bytes_per_block >> 1) >> (bytes_per_block >> 2));
  uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log_bpp + 7);
  uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log_bpp;
  uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4);
  return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
         log_bpp;
}
}  // namespace

uint64_t HashBaseLevel(const uint8_t* data, size_t size, uint32_t blocks_w, uint32_t blocks_h,
                       uint32_t bytes_per_block, uint32_t pitch_blocks, bool tiled) {
  std::vector<uint8_t> linear(size_t(blocks_w) * blocks_h * bytes_per_block);
  uint32_t tiled_pitch = (pitch_blocks + 31) & ~31u;
  for (uint32_t y = 0; y < blocks_h; ++y) {
    for (uint32_t x = 0; x < blocks_w; ++x) {
      size_t src = size_t(tiled ? TiledBlockOffset(x, y, tiled_pitch, bytes_per_block)
                                : size_t(y) * pitch_blocks + x) *
                   bytes_per_block;
      if (src + bytes_per_block > size) return 0;
      std::memcpy(&linear[(size_t(y) * blocks_w + x) * bytes_per_block], data + src, bytes_per_block);
    }
  }
  uint64_t hash = XXH3_64bits(linear.data(), linear.size());
  return hash ? hash : 1;
}

void ComputeSignature(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* sig_out) {
  for (uint32_t cy = 0; cy < kSigCells; ++cy) {
    uint32_t y0 = cy * height / kSigCells, y1 = std::max(y0 + 1, (cy + 1) * height / kSigCells);
    y1 = std::min(y1, height);
    for (uint32_t cx = 0; cx < kSigCells; ++cx) {
      uint32_t x0 = cx * width / kSigCells, x1 = std::max(x0 + 1, (cx + 1) * width / kSigCells);
      x1 = std::min(x1, width);
      uint64_t sum[4] = {};
      uint64_t n = 0;
      for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x, ++n)
          for (int k = 0; k < 4; ++k) sum[k] += rgba[(size_t(y) * width + x) * 4 + k];
      for (int k = 0; k < 4; ++k)
        sig_out[(cy * kSigCells + cx) * 4 + k] = uint8_t(n ? (sum[k] + n / 2) / n : 0);
    }
  }
}

namespace {
void Rgb565(uint16_t c, uint8_t* rgb) {
  rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  rgb[2] = uint8_t((c & 31) * 255 / 31);
}

// One little-endian BC1/BC2/BC3 block (kind 1/2/3) to 16 RGBA texels.
void DecodeBlock(const uint8_t* b, int kind, uint8_t out[16][4]) {
  const uint8_t* c = kind == 1 ? b : b + 8;
  uint16_t c0 = uint16_t(c[0] | c[1] << 8), c1 = uint16_t(c[2] | c[3] << 8);
  uint8_t pal[4][4];
  Rgb565(c0, pal[0]);
  Rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  bool four = kind != 1 || c0 > c1;
  for (int k = 0; k < 3; ++k) {
    pal[2][k] = four ? uint8_t((2 * pal[0][k] + pal[1][k]) / 3) : uint8_t((pal[0][k] + pal[1][k]) / 2);
    pal[3][k] = four ? uint8_t((pal[0][k] + 2 * pal[1][k]) / 3) : 0;
  }
  pal[2][3] = 255;
  pal[3][3] = four ? 255 : 0;
  uint32_t idx = uint32_t(c[4]) | uint32_t(c[5]) << 8 | uint32_t(c[6]) << 16 | uint32_t(c[7]) << 24;
  uint8_t alpha[16];
  if (kind == 2) {
    for (int i = 0; i < 16; ++i) alpha[i] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
  } else if (kind == 3) {
    uint8_t a0 = b[0], a1 = b[1], ap[8] = {a0, a1};
    if (a0 > a1) {
      for (int i = 1; i < 7; ++i) ap[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
    } else {
      for (int i = 1; i < 5; ++i) ap[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
      ap[6] = 0;
      ap[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) alpha[i] = ap[(bits >> (3 * i)) & 7];
  }
  for (int i = 0; i < 16; ++i) {
    const uint8_t* p = pal[(idx >> (2 * i)) & 3];
    out[i][0] = p[0];
    out[i][1] = p[1];
    out[i][2] = p[2];
    out[i][3] = kind == 1 ? p[3] : alpha[i];
  }
}
}  // namespace

bool GuestSignature(const uint8_t* data, size_t size, uint32_t width, uint32_t height,
                    xenos::TextureFormat format, uint32_t pitch_blocks, bool tiled, bool swap16,
                    uint8_t* sig_out) {
  int kind;
  uint32_t bpb, block;
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8: kind = 0; bpb = 4; block = 1; break;
    case xenos::TextureFormat::k_DXT1: kind = 1; bpb = 8; block = 4; break;
    case xenos::TextureFormat::k_DXT2_3: kind = 2; bpb = 16; block = 4; break;
    case xenos::TextureFormat::k_DXT4_5: kind = 3; bpb = 16; block = 4; break;
    default: return false;
  }
  uint32_t bw = (width + block - 1) / block, bh = (height + block - 1) / block;
  uint32_t pitch = tiled ? (pitch_blocks + 31) & ~31u : pitch_blocks;
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  uint8_t blk[16];
  uint8_t texels[16][4];
  for (uint32_t y = 0; y < bh; ++y) {
    for (uint32_t x = 0; x < bw; ++x) {
      size_t src = size_t(tiled ? TiledBlockOffset(x, y, pitch, bpb) : size_t(y) * pitch + x) * bpb;
      if (src + bpb > size) return false;
      std::memcpy(blk, data + src, bpb);
      if (kind == 0) {
        // Same byte order as lr_uiconv's decode of the 360 file: A R G B -> R G B A.
        uint8_t* d = &rgba[(size_t(y) * width + x) * 4];
        d[0] = blk[1];
        d[1] = blk[2];
        d[2] = blk[3];
        d[3] = blk[0];
        continue;
      }
      if (swap16)
        for (uint32_t k = 0; k + 1 < bpb; k += 2) std::swap(blk[k], blk[k + 1]);
      DecodeBlock(blk, kind, texels);
      for (uint32_t ty = 0; ty < 4; ++ty)
        for (uint32_t tx = 0; tx < 4; ++tx) {
          uint32_t px = x * 4 + tx, py = y * 4 + ty;
          if (px < width && py < height) std::memcpy(&rgba[(size_t(py) * width + px) * 4], texels[ty * 4 + tx], 4);
        }
    }
  }
  ComputeSignature(rgba.data(), width, height, sig_out);
  return true;
}

Registry& Registry::Get() {
  static Registry registry;
  return registry;
}

Registry::Registry() {
  const char* root = std::getenv("LR_TEXTURES");
  root_ = (root && root[0]) ? std::filesystem::path(root) : std::filesystem::path("textures");
  const char* dump = std::getenv("LR_TEXTURE_DUMP");
  dump_enabled_ = dump && dump[0] == '1';
  const char* replace = std::getenv("LR_TEXTURE_REPLACE");
  replace_enabled_ = !(replace && replace[0] == '0');
  const char* style = std::getenv("LR_UI_STYLE");
  ps3_ui_.store(style && (style[0] == 'p' || style[0] == 'P'));

  size_t user = 0;
  if (replace_enabled_) {
    // Texture overrides (replace\: sharp fonts and other replacements; mods_active\: texture
    // mods, installed by lr_texdump --install-mods at every start, after replace\ so they win)
    // are off for now: the beta has none. LR_TEXTURE_USER=1 turns them on. Only the PS3 button /
    // UI set below is used (the game's own Button Prompts setting switches to it).
    if (const char* user_on = std::getenv("LR_TEXTURE_USER"); user_on && user_on[0] == '1') {
      AddFolder(root_ / "replace", Set::kUser);
      AddFolder(root_ / "mods_active", Set::kUser);
    }
    user = replacements_.size();
    has_user_ = user != 0;
    AddFolder(root_ / "ps3ui", Set::kPs3Ui);
  }
  if (const char* capture = std::getenv("LR_SHADER_CAPTURE"); capture && capture[0]) {
    std::ifstream list(capture);
    std::string line;
    while (std::getline(list, line)) {
      bool ok;
      uint64_t hash = ParseHashPrefix(line, ok);
      unsigned w = 0, h = 0;
      if (!ok || !hash || line.size() < 18 || std::sscanf(line.c_str() + 17, "%ux%u", &w, &h) != 2 || !w || !h) continue;
      capture_hashes_.insert(hash);
      capture_sizes_.insert((uint64_t(w) << 32) | h);
    }
    REXLOG_INFO("LR capture: {} texture(s) of {} size(s) from {}", capture_hashes_.size(), capture_sizes_.size(),
                capture);
  }
  std::error_code ec;
  REXLOG_INFO("LR textures: root {}, {} replacement(s), {} PS3 UI texture(s), UI style {}{}{}",
              std::filesystem::absolute(root_, ec).string(), user, replacements_.size() - user,
              ps3_ui() ? "PS3" : "360", replace_enabled_ ? "" : " (replacement off)",
              dump_enabled_ ? ", dumping new textures to dump\\" : "");
}

void Registry::AddFolder(const std::filesystem::path& dir, Set set) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return;
  for (auto it = std::filesystem::recursive_directory_iterator(
           dir, std::filesystem::directory_options::skip_permission_denied, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    std::string ext = it->path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext != ".dds") continue;
    std::string stem = it->path().stem().string();
    bool ok;
    uint64_t hash = ParseHashPrefix(stem, ok);
    if (!ok) continue;
    // Optional guest size: <hash>_<w>x<h>...
    unsigned w = 0, h = 0;
    if (stem.size() > 17 && stem[16] == '_' && std::sscanf(stem.c_str() + 17, "%ux%u", &w, &h) == 2 && w && h) {
      sizes_.insert((uint64_t(w) << 32) | h);
    } else {
      any_size_ = true;
    }
    Entry e{it->path(), set, {}};
    std::filesystem::path sig_path = it->path();
    sig_path.replace_extension(".sig");
    std::filesystem::path src_check = it->path();
    src_check.replace_extension(".src");
    bool font = w && h && std::filesystem::is_regular_file(src_check, ec);
    if (std::FILE* f = font ? nullptr : std::fopen(sig_path.string().c_str(), "rb")) {
      uint8_t header[16];
      std::vector<uint8_t> sig(kSigBytes);
      if (std::fread(header, 1, 16, f) == 16 && std::memcmp(header, "LRSG", 4) == 0 &&
          std::fread(sig.data(), 1, kSigBytes, f) == kSigBytes && w && h) {
        e.sig = std::move(sig);
        sig_sizes_[(uint64_t(w) << 32) | h].push_back(hash);
      }
      std::fclose(f);
    }
    std::filesystem::path src_path = it->path();
    src_path.replace_extension(".src");
    if (w && h && std::filesystem::is_regular_file(src_path, ec)) {
      e.font_source = src_path;
      font_sizes_[(uint64_t(w) << 32) | h].push_back(hash);
    }
    // The user's own replacements win over the PS3 UI set.
    auto [entry, inserted] = replacements_.emplace(hash, e);
    if (!inserted && set == Set::kUser) entry->second = e;
  }
}

const std::vector<uint64_t>* Registry::FontAtlases(uint32_t width, uint32_t height) const {
  if (!replace_enabled_) return nullptr;
  auto it = font_sizes_.find((uint64_t(width) << 32) | height);
  return it != font_sizes_.end() ? &it->second : nullptr;
}

std::vector<uint64_t> Registry::AllFontAtlases() const {
  std::vector<uint64_t> all;
  if (!replace_enabled_) return all;
  for (const auto& size : font_sizes_) all.insert(all.end(), size.second.begin(), size.second.end());
  return all;
}

bool Registry::HasSignatures(uint32_t width, uint32_t height) const {
  return replace_enabled_ && sig_sizes_.count((uint64_t(width) << 32) | height) != 0;
}

uint64_t Registry::MatchSignature(uint32_t width, uint32_t height, const uint8_t* sig,
                                  float* diff_out) const {
  // The four byte orders a 32-bit texel can arrive in (8888 data with another endianness).
  static const int kOrders[4][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {3, 2, 1, 0}, {2, 3, 0, 1}};
  float best = 1e9f, second = 1e9f;
  uint64_t best_hash = 0;
  auto it = sig_sizes_.find((uint64_t(width) << 32) | height);
  if (it != sig_sizes_.end()) {
    for (uint64_t hash : it->second) {
      auto e = replacements_.find(hash);
      if (e == replacements_.end() || e->second.sig.size() != kSigBytes) continue;
      const uint8_t* cand = e->second.sig.data();
      float d_min = 1e9f;
      for (const auto& order : kOrders) {
        uint64_t total = 0;
        for (size_t c = 0; c < kSigBytes; c += 4)
          for (int k = 0; k < 4; ++k) total += uint64_t(std::abs(int(cand[c + order[k]]) - int(sig[c + k])));
        d_min = std::min(d_min, float(total) / float(kSigBytes));
      }
      if (d_min < best) {
        second = best;
        best = d_min;
        best_hash = hash;
      } else if (d_min < second) {
        second = d_min;
      }
    }
  }
  if (diff_out) *diff_out = best;
  // Close enough, and clearly closer than any other replacement of the same size.
  return best <= 4.0f && second - best >= 2.0f ? best_hash : 0;
}

bool Registry::LookupContent(uint64_t guest_hash, uint64_t& replacement_hash) {
  std::lock_guard<std::mutex> lock(content_mutex_);
  auto it = content_.find(guest_hash);
  if (it == content_.end()) return false;
  replacement_hash = it->second;
  return true;
}

void Registry::RememberContent(uint64_t guest_hash, uint64_t replacement_hash) {
  std::lock_guard<std::mutex> lock(content_mutex_);
  content_[guest_hash] = replacement_hash;
}

bool Registry::WantsHash(uint32_t width, uint32_t height) const {
  if (dump_enabled_) return true;
  if (capture_sizes_.count((uint64_t(width) << 32) | height)) return true;
  if (keyboard_on() && KeyboardSize(width, height)) return true;
  if (!replace_enabled_ || replacements_.empty()) return false;
  return any_size_ || sizes_.count((uint64_t(width) << 32) | height) != 0;
}

const Entry* Registry::Find(uint64_t hash) const {
  if (keyboard_on()) {
    std::lock_guard<std::mutex> lock(keyboard_mutex_);
    auto kb = keyboard_.find(hash);
    if (kb != keyboard_.end()) return kb->second;
  }
  if (!replace_enabled_) return nullptr;
  auto it = replacements_.find(hash);
  return it != replacements_.end() ? &it->second : nullptr;
}

void Registry::SetKeyboard(const std::filesystem::path& image, const std::vector<uint64_t>& hashes,
                           uint32_t width, uint32_t height) {
  {
    std::lock_guard<std::mutex> lock(keyboard_mutex_);
    keyboard_.clear();
    if (!image.empty()) {
      keyboard_entries_.push_back(Entry{image, Set::kKeyboard, {}, {}});
      for (uint64_t hash : hashes) keyboard_[hash] = &keyboard_entries_.back();
      keyboard_sizes_.insert((uint64_t(width) << 32) | height);
    }
  }
  keyboard_on_.store(!image.empty(), std::memory_order_relaxed);
  keyboard_generation_.fetch_add(1, std::memory_order_release);
}

bool Registry::KeyboardSize(uint32_t width, uint32_t height) const {
  std::lock_guard<std::mutex> lock(keyboard_mutex_);
  return keyboard_sizes_.count((uint64_t(width) << 32) | height) != 0;
}

void Registry::SetPs3Ui(bool ps3) {
  if (ps3_ui_.exchange(ps3) != ps3) {
    style_generation_.fetch_add(1, std::memory_order_release);
    REXLOG_INFO("LR textures: UI style {}", ps3 ? "PS3" : "360");
  }
}

bool Registry::ClaimDump(uint64_t hash) {
  std::lock_guard<std::mutex> lock(dump_mutex_);
  return dumped_.insert(hash).second;
}

// --- Font atlases (see FontComposite in replacement.h) ---
// The glyph reconstruction is the font tool's (lr_uiconv --fonts): an anti-aliased signed
// distance field from the 2-bit coverage, Catmull-Rom upsampling, a smoothstep about one
// screen pixel wide, box-filtered mips and the same BC1 encoder, so rebuilt blocks match the
// prebuilt atlas around them.
namespace {

struct Plane {
  int w = 0, h = 0;
  std::vector<float> v;
  Plane() = default;
  Plane(int w_, int h_, float fill = 0.f) : w(w_), h(h_), v(size_t(w_) * h_, fill) {}
  float& at(int x, int y) { return v[size_t(y) * w + x]; }
  float at(int x, int y) const { return v[size_t(y) * w + x]; }
  float clamped(int x, int y) const { return at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1)); }
};

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
          if (aq > 0.f) dout = std::min(dout, di + EdgeDistance(float(dx), float(dy), aq));
          if (aq < 1.f) din = std::min(din, di + EdgeDistance(float(dx), float(dy), 1.f - aq));
        }
      }
      out.v[i] = std::clamp(std::max(dout, 0.f) - std::max(din, 0.f), -cap, cap);
    }
  return out;
}

// Catmull-Rom upsampling by an integer factor (texel centres aligned).
Plane Upsample(const Plane& p, int scale) {
  auto weights = [](float t, float* wt) {
    float t2 = t * t, t3 = t2 * t;
    wt[0] = 0.5f * (-t3 + 2 * t2 - t);
    wt[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
    wt[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
    wt[3] = 0.5f * (t3 - t2);
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

Plane UpscaleCoverage(const Plane& c, int scale) {
  const int radius = 4;
  const float edge = std::max(1.f, scale * 0.5f);
  Plane up = Upsample(SignedDistance(c, radius), scale);
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

Plane Downsample(const Plane& p) {
  Plane o(std::max(1, p.w / 2), std::max(1, p.h / 2));
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x)
      o.at(x, y) = 0.25f * (p.clamped(2 * x, 2 * y) + p.clamped(2 * x + 1, 2 * y) + p.clamped(2 * x, 2 * y + 1) +
                            p.clamped(2 * x + 1, 2 * y + 1));
  return o;
}

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

// BC1 block from 16 RGBA texels (0..1); texels with alpha < 0.5 use the transparent index.
void EncodeBc1Block(const float (*px)[4], uint8_t* out) {
  bool transparent = false;
  float mean[3] = {0, 0, 0};
  int n = 0;
  for (int i = 0; i < 16; ++i) {
    if (px[i][3] < 0.5f) { transparent = true; continue; }
    for (int k = 0; k < 3; ++k) mean[k] += px[i][k];
    ++n;
  }
  uint16_t c0 = 0, c1 = 0;
  uint32_t indices = 0;
  if (!n) {
    c0 = 0; c1 = 0xFFFF; indices = 0xFFFFFFFFu;
  } else {
    for (float& m : mean) m /= float(n);
    float cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
      if (px[i][3] < 0.5f) continue;
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
      if (px[i][3] < 0.5f) continue;
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
      if (px[i][3] < 0.5f) {
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

// One guest DXT1 block (360 byte order) -> 16 RGBA texels (0..1), like the tool's decoder.
void DecodeGuestDxt1(const uint8_t* g, float (*px)[4]) {
  uint8_t b[8];
  for (int k = 0; k < 8; k += 2) { b[k] = g[k + 1]; b[k + 1] = g[k]; }
  uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
  auto c565 = [](uint16_t c, uint8_t* rgb) {
    rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    rgb[2] = uint8_t((c & 31) * 255 / 31);
  };
  uint8_t pal[4][4];
  c565(c0, pal[0]);
  c565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  bool four = c0 > c1;
  for (int k = 0; k < 3; ++k) {
    pal[2][k] = four ? uint8_t((2 * pal[0][k] + pal[1][k]) / 3) : uint8_t((pal[0][k] + pal[1][k]) / 2);
    pal[3][k] = four ? uint8_t((pal[0][k] + 2 * pal[1][k]) / 3) : 0;
  }
  pal[2][3] = 255;
  pal[3][3] = four ? 255 : 0;
  uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
  for (int i = 0; i < 16; ++i) {
    const uint8_t* c = pal[(idx >> (2 * i)) & 3];
    for (int k = 0; k < 4; ++k) px[i][k] = c[k] / 255.f;
  }
}

}  // namespace

bool UntileBaseLevel(const uint8_t* data, size_t size, uint32_t blocks_w, uint32_t blocks_h,
                     uint32_t bytes_per_block, uint32_t pitch_blocks, bool tiled,
                     std::vector<uint8_t>& out) {
  out.resize(size_t(blocks_w) * blocks_h * bytes_per_block);
  uint32_t tiled_pitch = (pitch_blocks + 31) & ~31u;
  for (uint32_t y = 0; y < blocks_h; ++y) {
    for (uint32_t x = 0; x < blocks_w; ++x) {
      size_t src = size_t(tiled ? TiledBlockOffset(x, y, tiled_pitch, bytes_per_block)
                                : size_t(y) * pitch_blocks + x) *
                   bytes_per_block;
      if (src + bytes_per_block > size) return false;
      std::memcpy(&out[(size_t(y) * blocks_w + x) * bytes_per_block], data + src, bytes_per_block);
    }
  }
  return true;
}

bool LoadFontSource(const std::filesystem::path& path, uint32_t width, uint32_t height,
                    std::vector<uint8_t>& blocks_out) {
  std::ifstream f(path, std::ios::binary);
  uint32_t header[5];
  if (!f || !f.read(reinterpret_cast<char*>(header), sizeof(header))) return false;
  if (std::memcmp(header, "LRSR", 4) != 0 || header[1] != 1 || header[2] != width ||
      header[3] != height || header[4] != 8) {
    return false;
  }
  blocks_out.resize(size_t((width + 3) / 4) * ((height + 3) / 4) * 8);
  return bool(f.read(reinterpret_cast<char*>(blocks_out.data()), std::streamsize(blocks_out.size())));
}

std::vector<uint32_t> FontGlyphBlocks(const std::vector<uint8_t>& source) {
  std::vector<uint32_t> glyph;
  for (size_t i = 0; i < source.size() / 8; ++i) {
    float px[16][4];
    DecodeGuestDxt1(&source[i * 8], px);
    bool empty = true;
    for (int t = 0; t < 16 && empty; ++t) empty = px[t][0] == 0.f && px[t][1] == 0.f && px[t][2] == 0.f;
    if (!empty) glyph.push_back(uint32_t(i));
  }
  return glyph;
}

float FontBlocksEqual(const std::vector<uint8_t>& guest, const std::vector<uint8_t>& source,
                      const std::vector<uint32_t>& glyph_blocks) {
  if (guest.size() != source.size() || glyph_blocks.empty()) return 0.f;
  size_t equal = 0;
  for (uint32_t i : glyph_blocks) equal += std::memcmp(&guest[size_t(i) * 8], &source[size_t(i) * 8], 8) == 0;
  return float(equal) / float(glyph_blocks.size());
}

float FontBlocksEqualSampled(const uint8_t* data, size_t size, uint32_t blocks_w,
                             uint32_t pitch_blocks, bool tiled, bool swap16,
                             const std::vector<uint8_t>& source,
                             const std::vector<uint32_t>& glyph_blocks, uint32_t stride) {
  if (glyph_blocks.empty() || !blocks_w || !stride) return 0.f;
  uint32_t tiled_pitch = (pitch_blocks + 31) & ~31u;
  size_t samples = 0, equal = 0;
  for (size_t k = 0; k < glyph_blocks.size(); k += stride) {
    uint32_t i = glyph_blocks[k];
    if (size_t(i) * 8 + 8 > source.size()) break;
    uint32_t x = i % blocks_w, y = i / blocks_w;
    size_t src = size_t(tiled ? TiledBlockOffset(x, y, tiled_pitch, 8) : size_t(y) * pitch_blocks + x) * 8;
    if (src + 8 > size) return 0.f;
    uint8_t block[8];
    std::memcpy(block, data + src, 8);
    if (swap16) {
      for (int b = 0; b < 8; b += 2) std::swap(block[b], block[b + 1]);
    }
    ++samples;
    equal += std::memcmp(block, &source[size_t(i) * 8], 8) == 0;
  }
  return samples ? float(equal) / float(samples) : 0.f;
}

std::vector<uint64_t> TileHashes(const std::vector<uint8_t>& blocks, uint32_t blocks_w,
                                 uint32_t blocks_h) {
  const uint32_t tiles_w = blocks_w / kFontTileBlocks, tiles_h = blocks_h / kFontTileBlocks;
  std::vector<uint64_t> hashes(size_t(tiles_w) * tiles_h, 0);
  if (blocks.size() < size_t(blocks_w) * blocks_h * 8) return hashes;
  std::vector<uint8_t> tile(size_t(kFontTileBlocks) * kFontTileBlocks * 8);
  for (uint32_t ty = 0; ty < tiles_h; ++ty)
    for (uint32_t tx = 0; tx < tiles_w; ++tx) {
      for (uint32_t y = 0; y < kFontTileBlocks; ++y)
        std::memcpy(&tile[size_t(y) * kFontTileBlocks * 8],
                    &blocks[(size_t(ty * kFontTileBlocks + y) * blocks_w + tx * kFontTileBlocks) * 8],
                    size_t(kFontTileBlocks) * 8);
      hashes[size_t(ty) * tiles_w + tx] = XXH3_64bits(tile.data(), tile.size());
    }
  return hashes;
}

Image MakeBc1Image(uint32_t width, uint32_t height, uint32_t levels) {
  Image image;
  image.width = width;
  image.height = height;
  image.dxgi_format = kDxgiBC1Unorm;
  size_t total = 0;
  for (uint32_t m = 0; m < levels; ++m) {
    uint32_t w = std::max(1u, width >> m), h = std::max(1u, height >> m);
    Image::Level lv{total, ((w + 3) / 4) * 8, (h + 3) / 4};
    image.levels.push_back(lv);
    total += size_t(lv.row_pitch) * lv.rows;
  }
  image.data.assign(total, 0);
  return image;
}

void CopyImageTile(const Image& src, uint32_t src_tx, uint32_t src_ty, Image& dst, uint32_t dst_tx,
                   uint32_t dst_ty, uint32_t tile_texels) {
  for (size_t m = 0; m < dst.levels.size() && m < src.levels.size(); ++m) {
    uint32_t tile_blocks = (tile_texels >> m) / 4;
    if (!tile_blocks) break;
    const Image::Level& sl = src.levels[m];
    const Image::Level& dl = dst.levels[m];
    for (uint32_t y = 0; y < tile_blocks; ++y) {
      size_t sy = size_t(src_ty) * tile_blocks + y, dy = size_t(dst_ty) * tile_blocks + y;
      size_t sx = size_t(src_tx) * tile_blocks * 8, dx = size_t(dst_tx) * tile_blocks * 8;
      if (sy >= sl.rows || dy >= dl.rows || sx + tile_blocks * 8 > sl.row_pitch ||
          dx + tile_blocks * 8 > dl.row_pitch) {
        break;
      }
      std::memcpy(&dst.data[dl.offset + dy * dl.row_pitch + dx], &src.data[sl.offset + sy * sl.row_pitch + sx],
                  size_t(tile_blocks) * 8);
    }
  }
}

namespace {
// A BC1 block of one texel color (alpha < 0.5: transparent).
void SolidBc1Block(const float* rgba, uint8_t* out) {
  if (rgba[3] < 0.5f) {
    const uint8_t transparent[8] = {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::memcpy(out, transparent, 8);
    return;
  }
  auto q = [](float v, int bits) { return std::clamp(int(v * float((1 << bits) - 1) + 0.5f), 0, (1 << bits) - 1); };
  uint16_t c = uint16_t(q(rgba[0], 5) << 11 | q(rgba[1], 6) << 5 | q(rgba[2], 5));
  // c0 == c1 selects the 3-color mode; index 0 is c0.
  const uint8_t block[8] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c), uint8_t(c >> 8), 0, 0, 0, 0};
  std::memcpy(out, block, 8);
}
}  // namespace

void UpscaleGuestTile(const std::vector<uint8_t>& guest, uint32_t guest_blocks_w, uint32_t tx, uint32_t ty,
                      Image& dst, uint32_t scale) {
  const uint32_t tile_texels = kFontTileBlocks * 4;
  for (size_t m = 0; m < dst.levels.size(); ++m) {
    uint32_t s = scale >> m;  // output texels per guest texel at this level
    if (!s) break;
    const Image::Level& lv = dst.levels[m];
    uint32_t out_blocks = tile_texels * s / 4;
    for (uint32_t oy = 0; oy < out_blocks; ++oy) {
      size_t row = size_t(ty) * out_blocks + oy;
      if (row >= lv.rows) break;
      for (uint32_t ox = 0; ox < out_blocks; ++ox) {
        size_t col = (size_t(tx) * out_blocks + ox) * 8;
        if (col + 8 > lv.row_pitch) break;
        uint8_t* out = &dst.data[lv.offset + row * lv.row_pitch + col];
        // Guest texel of the output block's first texel.
        uint32_t gx = tx * tile_texels + ox * 4 / s, gy = ty * tile_texels + oy * 4 / s;
        const uint8_t* g = &guest[(size_t(gy / 4) * guest_blocks_w + gx / 4) * 8];
        float px[16][4];
        DecodeGuestDxt1(g, px);
        if (s >= 4) {
          SolidBc1Block(px[(gy % 4) * 4 + gx % 4], out);
        } else if (s == 1) {
          for (int k = 0; k < 8; k += 2) { out[k] = g[k + 1]; out[k + 1] = g[k]; }
        } else {
          float up[16][4];
          for (int i = 0; i < 16; ++i) {
            uint32_t x = (gx % 4) + (i % 4) / s, y = (gy % 4) + (i / 4) / s;
            std::memcpy(up[i], px[std::min(y, 3u) * 4 + std::min(x, 3u)], sizeof(up[i]));
          }
          EncodeBc1Block(up, out);
        }
      }
    }
  }
}

uint32_t FontComposite(Image& image, const std::vector<uint8_t>& guest,
                       const std::vector<uint8_t>& source, uint32_t width, uint32_t height) {
  if (image.dxgi_format != kDxgiBC1Unorm || !width || !height || image.width % width ||
      image.height % height || image.width / width != image.height / height ||
      guest.size() != source.size()) {
    return 0;
  }
  const int scale = int(image.width / width);
  const int bw = int(width + 3) / 4, bh = int(height + 3) / 4;
  if (guest.size() != size_t(bw) * bh * 8) return 0;
  // Tiles of 8x8 guest blocks (32 texels) with any changed block are rebuilt, from a region with
  // an 8-texel margin (the distance field reaches 4 texels, the cubic filter 2 more), which
  // keeps every region origin aligned for the box-filtered mips.
  const int kTile = 8, kMargin = 8;
  const int tw = (bw + kTile - 1) / kTile, th = (bh + kTile - 1) / kTile;
  std::vector<uint8_t> dirty_tiles(size_t(tw) * th, 0);
  uint32_t dirty_blocks = 0;
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      size_t i = size_t(by) * bw + bx;
      if (std::memcmp(&guest[i * 8], &source[i * 8], 8) != 0) {
        ++dirty_blocks;
        dirty_tiles[size_t(by / kTile) * tw + bx / kTile] = 1;
      }
    }
  for (int ty = 0; ty < th; ++ty)
    for (int tx = 0; tx < tw; ++tx) {
      if (!dirty_tiles[size_t(ty) * tw + tx]) continue;
      // Guest texel region (block aligned).
      int x0 = std::max(0, tx * kTile * 4 - kMargin), y0 = std::max(0, ty * kTile * 4 - kMargin);
      int x1 = std::min(int(width), (tx + 1) * kTile * 4 + kMargin);
      int y1 = std::min(int(height), (ty + 1) * kTile * 4 + kMargin);
      int rw = x1 - x0, rh = y1 - y0;
      Plane ch[4];
      for (Plane& p : ch) p = Plane(rw, rh);
      for (int by = y0 / 4; by < (y1 + 3) / 4; ++by)
        for (int bx = x0 / 4; bx < (x1 + 3) / 4; ++bx) {
          float px[16][4];
          DecodeGuestDxt1(&guest[(size_t(by) * bw + bx) * 8], px);
          for (int i = 0; i < 16; ++i) {
            int x = bx * 4 + i % 4 - x0, y = by * 4 + i / 4 - y0;
            if (x < 0 || y < 0 || x >= rw || y >= rh) continue;
            for (int k = 0; k < 4; ++k) ch[k].at(x, y) = px[i][k];
          }
        }
      // Per channel: constant, a copy of an earlier channel, or coverage to reconstruct.
      std::vector<Plane> planes;
      int plane_of[4];
      for (int k = 0; k < 4; ++k) {
        plane_of[k] = -1;
        for (int j = 0; j < k && plane_of[k] < 0; ++j)
          if (ch[j].v == ch[k].v) plane_of[k] = plane_of[j];
        if (plane_of[k] >= 0) continue;
        plane_of[k] = int(planes.size());
        float first = ch[k].v[0];
        if (std::all_of(ch[k].v.begin(), ch[k].v.end(), [&](float v) { return v == first; })) {
          planes.emplace_back(1, 1, first);
        } else {
          planes.push_back(UpscaleCoverage(ch[k], scale));
        }
      }
      // Level 0 region origin in output texels; the tile interior to write.
      int ox = x0 * scale, oy = y0 * scale;
      int ix0 = tx * kTile * 4 * scale, iy0 = ty * kTile * 4 * scale;
      int ix1 = std::min(int(image.width), (tx + 1) * kTile * 4 * scale);
      int iy1 = std::min(int(image.height), (ty + 1) * kTile * 4 * scale);
      for (size_t level = 0; level < image.levels.size(); ++level) {
        if (level) {
          for (Plane& p : planes)
            if (p.w > 1 || p.h > 1) p = Downsample(p);
          ox >>= 1; oy >>= 1; ix0 >>= 1; iy0 >>= 1; ix1 >>= 1; iy1 >>= 1;
        }
        // Stop once the tile no longer covers whole blocks (the smallest mips keep the
        // prebuilt glyphs; text is never drawn that small).
        if ((ix0 & 3) || (iy0 & 3) || ix1 - ix0 < 4 || iy1 - iy0 < 4) break;
        const Image::Level& lv = image.levels[level];
        for (int by = iy0 / 4; by < iy1 / 4; ++by) {
          if (uint32_t(by) >= lv.rows) break;
          for (int bx = ix0 / 4; bx < ix1 / 4; ++bx) {
            if (size_t(bx) * 8 + 8 > lv.row_pitch) break;
            float px[16][4];
            for (int i = 0; i < 16; ++i)
              for (int k = 0; k < 4; ++k) {
                const Plane& p = planes[plane_of[k]];
                px[i][k] = p.w == 1 && p.h == 1 ? p.v[0]
                                                : p.clamped(bx * 4 + i % 4 - ox, by * 4 + i / 4 - oy);
              }
            size_t at = lv.offset + size_t(by) * lv.row_pitch + size_t(bx) * 8;
            if (at + 8 <= image.data.size()) EncodeBc1Block(px, &image.data[at]);
          }
        }
      }
    }
  return dirty_blocks;
}

}  // namespace rex::graphics::texture_replacement

// Live UI style switch for the game executable (looked up with GetProcAddress / dlsym).
extern "C" {
#if defined(_WIN32)
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int LrSetUiStyle(int ps3) {
  rex::graphics::texture_replacement::Registry::Get().SetPs3Ui(ps3 != 0);
  return ps3 != 0;
}
#if defined(_WIN32)
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int LrSetKeyboardIcons(const uint8_t* rgba, uint32_t width, uint32_t height, const uint64_t* hashes,
                       int hash_count, uint32_t guest_width, uint32_t guest_height) {
  // Keyboard mode's button icon sheet (R8G8B8A8, one level), standing in for the guest textures
  // with these hashes; rgba = nullptr turns it off.
  using namespace rex::graphics::texture_replacement;
  Registry& registry = Registry::Get();
  if (!rgba || !width || !height || hash_count <= 0) {
    registry.SetKeyboard({}, {}, 0, 0);
    REXLOG_INFO("LR textures: keyboard icons off");
    return 0;
  }
  static std::atomic<uint32_t> serial{0};
  std::filesystem::path dir = registry.root() / "keyboard";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  // Old files of this run are left for the image cache; earlier runs' are removed.
  uint32_t n = serial.fetch_add(1);
  if (n == 0) {
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
      if (it->path().extension() == ".dds") std::filesystem::remove(it->path(), ec);
    }
  }
  std::filesystem::path path = dir / ("keyboard_icons_" + std::to_string(n) + ".dds");
  if (!SaveDds(path, width, height, kDxgiR8G8B8A8Unorm, rgba, size_t(width) * height * 4)) {
    REXLOG_WARN("LR textures: could not write {}", path.string());
    return -1;
  }
  registry.SetKeyboard(path, std::vector<uint64_t>(hashes, hashes + hash_count), guest_width, guest_height);
  REXLOG_INFO("LR textures: keyboard icons {} ({}x{}) for {} texture(s)", path.filename().string(), width, height,
              hash_count);
  return 1;
}
#if defined(_WIN32)
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int LrToggleUiStyle() {
  auto& registry = rex::graphics::texture_replacement::Registry::Get();
  registry.SetPs3Ui(!registry.ps3_ui());
  return registry.ps3_ui() ? 1 : 0;
}
}
