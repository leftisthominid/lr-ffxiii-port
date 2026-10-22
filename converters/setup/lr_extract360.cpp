// Lightning Returns port - copies the 360 game files out of a disc image or a Games on Demand
// package (what the console writes to a USB drive when you download the game).
//
//   lr_extract360 <game.iso | GoD header file | USB drive or folder> <out folder>
//
// For a folder, it looks for Content\<profile>\5351081B\00007000\<header file> below it.
// Disc image: XDVDFS file system at one of the known game partition offsets.
// Games on Demand: the same file system stored in <header>.data\Data0000.. with hash blocks
// in between (block mapping follows xenia's SVOD reader, BSD licence).
// The source is only read.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static const char kMagic[] = "MICROSOFT*XBOX*MEDIA";

struct Reader {
  std::vector<std::unique_ptr<std::ifstream>> files;
  std::vector<uint64_t> sizes;
  void open(const fs::path& p) {
    auto f = std::make_unique<std::ifstream>(p, std::ios::binary);
    if (!*f) throw std::runtime_error("cannot open " + p.string());
    files.push_back(std::move(f));
    sizes.push_back(fs::file_size(p));
  }
  void read(size_t file, uint64_t off, void* dst, size_t n) {
    if (file >= files.size() || off + n > sizes[file]) throw std::runtime_error("read past the end of the image (incomplete copy?)");
    auto& f = *files[file];
    f.clear();
    f.seekg(std::streamoff(off));
    f.read(static_cast<char*>(dst), std::streamsize(n));
    if (!f) throw std::runtime_error("read failed");
  }
};

// Maps a file system sector (0x800 bytes) to (file, offset).
struct Image {
  Reader r;
  bool god = false;
  uint64_t game_offset = 0;      // disc image
  uint32_t start_block = 0;      // GoD
  bool egdf = false, single = false;
  uint64_t base = 0;
  uint32_t root_sector = 0, root_size = 0;

  void locate(uint32_t block, size_t* file, uint64_t* off) const {
    if (!god) {
      *file = 0;
      *off = game_offset + uint64_t(block) * 0x800;
      return;
    }
    const uint64_t kBlocksPerL0 = 0x198, kL0PerL1 = 0xA1C4, kBlocksPerFile = 0x14388, kMaxFile = 0xA290000;
    uint64_t b = uint64_t(block) - uint64_t(start_block) * 2;
    if (egdf) b += 2;
    uint64_t file_block = b % kBlocksPerFile;
    size_t index = size_t(b / kBlocksPerFile);
    uint64_t l0 = file_block / kBlocksPerL0 + 1;
    uint64_t offset = l0 * 0x1000 + (l0 / kL0PerL1 + 1) * 0x1000;
    if (single) offset += base;
    uint64_t addr = file_block * 0x800 + offset;
    if (addr >= kMaxFile) {
      index++;
      addr = addr % kMaxFile + 0x2000;
    }
    *file = index;
    *off = addr;
  }
  void read_sector(uint32_t block, uint8_t* dst) {
    size_t f;
    uint64_t o;
    locate(block, &f, &o);
    r.read(f, o, dst, 0x800);
  }
};

static bool open_iso(Image& img, const fs::path& p) {
  img.r.open(p);
  static const uint64_t offsets[] = {0x0, 0xFB20, 0x20600, 0x2080000, 0xFD90000};
  for (uint64_t o : offsets) {
    uint8_t vd[0x800];
    if (o + 33 * 0x800 > img.r.sizes[0]) continue;
    img.r.read(0, o + 32 * 0x800, vd, sizeof(vd));
    if (memcmp(vd, kMagic, 20)) continue;
    img.game_offset = o;
    img.root_sector = le32(vd + 20);
    img.root_size = le32(vd + 24);
    return true;
  }
  return false;
}

static bool open_god(Image& img, const fs::path& header) {
  std::vector<uint8_t> h(0x3B0);
  {
    std::ifstream f(header, std::ios::binary);
    f.read(reinterpret_cast<char*>(h.data()), std::streamsize(h.size()));
    if (!f) return false;
  }
  if (memcmp(h.data(), "LIVE", 4) && memcmp(h.data(), "PIRS", 4) && memcmp(h.data(), "CON ", 4)) return false;
  if (be32(&h[0x3A9]) != 1) throw std::runtime_error(header.filename().string() + " is a package but not a Games on Demand one");
  uint32_t title = be32(&h[0x360]);
  if (title != 0x5351081B) printf("  note: title id %08X is not Lightning Returns (5351081B)\n", title);
  uint32_t count = be32(&h[0x39D]);
  img.egdf = (h[0x379 + 0x18] >> 6) & 1;
  img.start_block = h[0x379 + 0x1C] | h[0x379 + 0x1D] << 8 | h[0x379 + 0x1E] << 16;  // u24 LE
  fs::path data = header;
  data += ".data";
  std::vector<fs::path> parts;
  if (fs::is_directory(data))
    for (auto& e : fs::directory_iterator(data))
      if (e.is_regular_file()) parts.push_back(e.path());
  std::sort(parts.begin(), parts.end());
  if (parts.size() != count)
    throw std::runtime_error("expected " + std::to_string(count) + " Data files in " + data.string() + ", found " + std::to_string(parts.size()));
  for (auto& p : parts) img.r.open(p);
  img.god = true;
  img.single = count == 1;
  uint8_t vd[0x800];
  uint64_t magic_at;
  if (img.egdf) {
    magic_at = 0x2000;
  } else {
    img.r.read(0, 0x12000, vd, 20);
    if (!memcmp(vd, kMagic, 20)) {
      magic_at = 0x12000;
      img.base = 0x10000;
    } else {
      magic_at = img.single ? 0xD000 : 0x2000;
      img.base = 0xB000;
      if (!img.single) img.base = 0;
    }
  }
  img.r.read(0, magic_at, vd, 0x800);
  if (memcmp(vd, kMagic, 20)) throw std::runtime_error("no Xbox file system found in the Games on Demand data");
  img.root_sector = le32(vd + 20);
  img.root_size = le32(vd + 24);
  return true;
}

struct FileEntry {
  std::string path;
  uint32_t sector, size;
};

static void walk(Image& img, uint32_t sector, uint32_t size, const std::string& prefix, std::vector<FileEntry>& out, int depth) {
  if (depth > 32) throw std::runtime_error("directory tree too deep");
  std::vector<uint8_t> dir((size + 0x7FF) / 0x800 * 0x800);
  for (size_t i = 0; i < dir.size() / 0x800; i++) img.read_sector(sector + uint32_t(i), &dir[i * 0x800]);
  std::vector<uint32_t> todo{0};
  std::vector<bool> seen(dir.size() / 4 + 1);
  while (!todo.empty()) {
    uint32_t ord = todo.back();
    todo.pop_back();
    size_t at = size_t(ord) * 4;
    if (at + 14 > dir.size() || seen[ord]) continue;
    seen[ord] = true;
    const uint8_t* e = &dir[at];
    uint16_t l = e[0] | e[1] << 8, r = e[2] | e[3] << 8;
    if (l == 0xFFFF && r == 0xFFFF) continue;  // padding
    uint32_t s = le32(e + 4), n = le32(e + 8);
    uint8_t attr = e[12], len = e[13];
    if (at + 14 + len > dir.size()) continue;
    std::string name(reinterpret_cast<const char*>(e + 14), len);
    if (l && l != 0xFFFF) todo.push_back(l);
    if (r && r != 0xFFFF) todo.push_back(r);
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) continue;
    std::string path = prefix.empty() ? name : prefix + "/" + name;
    if (attr & 0x10) {
      if (n) walk(img, s, n, path, out, depth + 1);
      else out.push_back({path + "/", 0, 0});
    } else {
      out.push_back({path, s, n});
    }
  }
}

static fs::path find_god(const fs::path& root) {
  std::vector<fs::path> hits;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it.depth() > 6) { it.disable_recursion_pending(); continue; }
    const fs::path& p = it->path();
    if (!it->is_regular_file(ec)) continue;
    auto parent = p.parent_path();
    if (parent.filename() != "00007000" || parent.parent_path().filename().string().find("5351081B") == std::string::npos) continue;
    fs::path data = p;
    data += ".data";
    if (fs::is_directory(data)) hits.push_back(p);
  }
  if (hits.empty()) throw std::runtime_error("no Lightning Returns Games on Demand package (Content\\...\\5351081B\\00007000\\...) under " + root.string());
  return hits[0];
}

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: lr_extract360 <game.iso | GoD header file | USB drive or folder> <out folder>\n");
    return 1;
  }
  try {
    fs::path src = fs::u8path(argv[1]), dst = fs::u8path(argv[2]);
    Image img;
    if (fs::is_directory(src)) {
      src = find_god(src);
      printf("Games on Demand package: %s\n", src.string().c_str());
      open_god(img, src);
    } else {
      char magic[4] = {};
      {
        std::ifstream f(src, std::ios::binary);
        if (!f) throw std::runtime_error("cannot open " + src.string());
        f.read(magic, 4);
      }
      if (!memcmp(magic, "LIVE", 4) || !memcmp(magic, "PIRS", 4) || !memcmp(magic, "CON ", 4)) {
        printf("Games on Demand package: %s\n", src.string().c_str());
        open_god(img, src);
      } else {
        printf("Disc image: %s\n", src.string().c_str());
        if (!open_iso(img, src)) throw std::runtime_error("no Xbox 360 file system in " + src.string() + " (not a 360 disc image?)");
      }
    }
    std::vector<FileEntry> files;
    walk(img, img.root_sector, img.root_size, "", files, 0);
    uint64_t total = 0;
    for (auto& f : files) total += f.size;
    printf("%zu files, %.1f GB\n", files.size(), total / 1073741824.0);
    std::vector<uint8_t> buf(0x800 * 1024);
    uint64_t done = 0, next = 0;
    for (auto& f : files) {
      fs::path out = dst / fs::u8path(f.path);
      if (f.path.back() == '/') { fs::create_directories(out); continue; }
      fs::create_directories(out.parent_path());
      std::ofstream o(out, std::ios::binary | std::ios::trunc);
      if (!o) throw std::runtime_error("cannot write " + out.string());
      uint32_t left = f.size, sec = f.sector;
      while (left) {
        // runs of sectors that are contiguous in the source are read in one go
        size_t fi0;
        uint64_t off0;
        img.locate(sec, &fi0, &off0);
        uint32_t run = 1;
        while (run < buf.size() / 0x800 && uint64_t(run) * 0x800 < left) {
          size_t fi;
          uint64_t off;
          img.locate(sec + run, &fi, &off);
          if (fi != fi0 || off != off0 + uint64_t(run) * 0x800) break;
          run++;
        }
        uint32_t n = std::min<uint64_t>(left, uint64_t(run) * 0x800);
        img.r.read(fi0, off0, buf.data(), n);
        o.write(reinterpret_cast<const char*>(buf.data()), n);
        left -= n;
        sec += run;
        done += n;
        if (done >= next) {
          printf("  %5.1f / %.1f GB\n", done / 1073741824.0, total / 1073741824.0);
          fflush(stdout);
          next = done + (512ull << 20);
        }
      }
      if (!o) throw std::runtime_error("write failed: " + out.string());
    }
    printf("done: %zu files, %.1f GB in %s\n", files.size(), total / 1073741824.0, dst.string().c_str());
    return 0;
  } catch (const std::exception& e) {
    fprintf(stderr, "error: %s\n", e.what());
    return 2;
  }
}
