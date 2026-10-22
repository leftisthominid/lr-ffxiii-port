#pragma once
/**
 * @file        rex/graphics/pipeline/texture/replacement.h
 * @brief       Hash-keyed texture replacement and dumping (Lightning Returns port)
 *
 * A guest texture is identified by the 64-bit XXH3 hash of its base level's blocks in plain
 * row order: un-tiled, tightly packed (no row padding), bytes not swapped - the game's own
 * data, independent of how it is laid out in memory, so tools can compute it from the
 * game files (lr_uiconv does, for the PS3 UI set). A replacement is a
 * DDS file named <16 hex digits of the hash>[anything].dds anywhere under
 * <root>/replace. With dumping on, every distinct base level the game uploads is written
 * to <root>/dump/<hash>_<width>x<height>_<format>.dds (untiled and byte-swapped, so the
 * file is the guest texture as an ordinary DDS) for building replacement packs.
 *
 * Replacement sets: <root>/replace (always used) and <root>/ps3ui (the PS3 versions of the
 * 360 UI textures that differ, used while the UI style is "ps3"). The style can be switched
 * while the game runs (LrSetUiStyle / LrToggleUiStyle, exported from the plugin); bindings
 * are re-resolved on the next draw.
 *
 * Only textures whose size matches a replacement are hashed: names may carry the guest size
 * after the hash (<hash>_<w>x<h>...); a file without one makes every texture a candidate.
 *
 * Environment:
 *   LR_TEXTURES      root folder (default: "textures" in the working directory)
 *   LR_TEXTURE_DUMP  1 = dump base levels
 *   LR_TEXTURE_REPLACE 0 = ignore <root>/replace and <root>/ps3ui
 *   LR_UI_STYLE      ps3 = start with the PS3 UI (default 360)
 */

#include <atomic>
#include <deque>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics::texture_replacement {

// DXGI_FORMAT values used by replacement images (shared by both backends).
enum : uint32_t {
  kDxgiR8G8B8A8Unorm = 28,
  kDxgiBC1Unorm = 71,
  kDxgiBC2Unorm = 74,
  kDxgiBC3Unorm = 77,
  kDxgiBC4Unorm = 80,
  kDxgiBC5Unorm = 83,
  kDxgiB8G8R8A8Unorm = 87,
  kDxgiBC7Unorm = 98,
};

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t dxgi_format = 0;
  // Per mip level: offset into data, bytes per row of blocks, rows of blocks.
  struct Level {
    size_t offset;
    uint32_t row_pitch;
    uint32_t rows;
  };
  std::vector<Level> levels;
  std::vector<uint8_t> data;

  bool IsBlockCompressed() const;
};

// The identifying hash (see the top of this file). `data` is the base level in guest memory,
// `size` the bytes that may be read; pitch_blocks is the row pitch in blocks.
uint64_t HashBaseLevel(const uint8_t* data, size_t size, uint32_t blocks_w, uint32_t blocks_h,
                       uint32_t bytes_per_block, uint32_t pitch_blocks, bool tiled);

// Block size in texels (1 or 4) and bytes per block for a supported DXGI format, or 0.
uint32_t BytesPerBlock(uint32_t dxgi_format);
uint32_t BlockSize(uint32_t dxgi_format);

// Reads a DDS file (DX10 or legacy header; BC1-5, BC7, 32-bit RGBA/BGRA; 2D only, no
// arrays). sRGB variants are returned as their UNORM format.
bool LoadDds(const std::filesystem::path& path, Image& image_out, std::string& error_out);
bool SaveDds(const std::filesystem::path& path, uint32_t width, uint32_t height,
             uint32_t dxgi_format, const void* data, size_t size);

// Content signature: RGBA averages over a 16x16 grid of cells (cell x covers texels
// [x * w / 16, max(x * w / 16 + 1, (x + 1) * w / 16)), same for y). The tools write the
// signature of the original 360 texture next to each replacement (<same name>.sig: "LRSG",
// u32 version 1, u32 width, u32 height, 1024 bytes). When the hash of a guest texture finds no
// replacement but a replacement of the same size has a signature, the guest texture is
// decoded and matched by content instead, so a difference in how the guest data is laid out
// in memory cannot hide a replacement.
constexpr uint32_t kSigCells = 16;
constexpr size_t kSigBytes = kSigCells * kSigCells * 4;
void ComputeSignature(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* sig_out);
// Decodes a guest base level (8_8_8_8, DXT1, DXT2/3, DXT4/5) to RGBA and computes its
// signature. swap16: DXT data in the 360's 16-bit byte order. False for other formats.
bool GuestSignature(const uint8_t* data, size_t size, uint32_t width, uint32_t height,
                    xenos::TextureFormat format, uint32_t pitch_blocks, bool tiled, bool swap16,
                    uint8_t* sig_out);

// Font atlases (Lightning Returns). The game draws text from DXT1 glyph atlases whose free
// space it fills with more glyphs at runtime, so the atlas in memory never hashes like the file.
// The font tool writes the original 360 atlas next to each font replacement (<same name>.src:
// "LRSR", u32 version 1, u32 width, u32 height, u32 bytes per block, then the untiled blocks in
// guest byte order). A guest texture of that size whose blocks mostly equal the original's is
// that atlas: the blocks the game changed are rebuilt from guest memory with the same glyph
// reconstruction the tool uses and written over the replacement's mips.
bool UntileBaseLevel(const uint8_t* data, size_t size, uint32_t blocks_w, uint32_t blocks_h,
                     uint32_t bytes_per_block, uint32_t pitch_blocks, bool tiled,
                     std::vector<uint8_t>& out);
bool LoadFontSource(const std::filesystem::path& path, uint32_t width, uint32_t height,
                    std::vector<uint8_t>& blocks_out);
// The glyph (not empty) blocks of an original atlas, and the fraction of them `guest` still has
// (both untiled, same size).
std::vector<uint32_t> FontGlyphBlocks(const std::vector<uint8_t>& source);
float FontBlocksEqual(const std::vector<uint8_t>& guest, const std::vector<uint8_t>& source,
                      const std::vector<uint32_t>& glyph_blocks);
// The same over every `stride`th glyph block, read straight from guest memory in the given
// layout (a quick test before untiling the whole texture).
float FontBlocksEqualSampled(const uint8_t* data, size_t size, uint32_t blocks_w,
                             uint32_t pitch_blocks, bool tiled, bool swap16,
                             const std::vector<uint8_t>& source,
                             const std::vector<uint32_t>& glyph_blocks, uint32_t stride);
// Rebuilds the blocks of a BC1 replacement `image` (an integer multiple of width x height)
// where `guest` differs from `source`. Returns the number of guest blocks that differed.
uint32_t FontComposite(Image& image, const std::vector<uint8_t>& guest,
                       const std::vector<uint8_t>& source, uint32_t width, uint32_t height);

// Font windows. The game does not always sample an atlas through a texture at the atlas's own
// address: Lightning Returns draws its text through a texture of the atlas's size that starts
// 0x1CA000 bytes before the atlas, so the glyphs are in its last tiles. A tiled texture's 128x128
// tiles (32x32 blocks, 8 KB for DXT1) are self-contained in memory, so each tile of such a window
// is either a tile of an atlas (found by content, then mapped by address) or other data.
constexpr uint32_t kFontTileBlocks = 32;
// XXH3 of each whole 32x32-block tile of untiled 8-byte blocks, row-major.
std::vector<uint64_t> TileHashes(const std::vector<uint8_t>& blocks, uint32_t blocks_w, uint32_t blocks_h);
// An all-zero BC1 image with `levels` mip levels.
Image MakeBc1Image(uint32_t width, uint32_t height, uint32_t levels);
// Copies a tile (tile_texels square at level 0, halved per level) of every level dst has.
void CopyImageTile(const Image& src, uint32_t src_tx, uint32_t src_ty, Image& dst, uint32_t dst_tx,
                   uint32_t dst_ty, uint32_t tile_texels);
// Fills tile (tx, ty) of dst (scale x the guest size) with the guest DXT1 texels enlarged
// (nearest), for the levels down to the guest's size.
void UpscaleGuestTile(const std::vector<uint8_t>& guest, uint32_t guest_blocks_w, uint32_t tx, uint32_t ty,
                      Image& dst, uint32_t scale);

enum class Set : uint8_t { kUser = 0, kPs3Ui = 1, kKeyboard = 2 };

struct Entry {
  std::filesystem::path path;
  Set set = Set::kUser;
  std::vector<uint8_t> sig;  // kSigBytes, or empty
  std::filesystem::path font_source;  // .src next to a font atlas replacement, or empty
};

class Registry {
 public:
  static Registry& Get();

  bool replace_enabled() const { return replace_enabled_; }
  // Whether any user replacement (replace\, mods_active\) is loaded; only those can stand in for
  // textures the GPU wrote, so without them nothing is copied back from the GPU to be hashed.
  bool has_user() const { return has_user_; }
  bool dump_enabled() const { return dump_enabled_; }
  const std::filesystem::path& root() const { return root_; }

  // Whether a guest texture of this size can have a replacement (or must be dumped), i.e.
  // whether it is worth hashing.
  bool WantsHash(uint32_t width, uint32_t height) const;
  // The replacement for this hash, or nullptr.
  const Entry* Find(uint64_t hash) const;
  // True the first time a hash is offered (so each texture is dumped once).
  bool ClaimDump(uint64_t hash);
  // Content matching (see kSigCells): whether replacements of this size have signatures;
  // the best match for a guest signature (replacement hash, or 0 if none is close enough or
  // two are equally close) and its mean difference; and a memory of earlier results by
  // guest hash, so each guest texture is decoded once.
  bool HasSignatures(uint32_t width, uint32_t height) const;
  uint64_t MatchSignature(uint32_t width, uint32_t height, const uint8_t* sig, float* diff_out) const;
  bool LookupContent(uint64_t guest_hash, uint64_t& replacement_hash);
  // Font atlas replacements of this size (their hashes), or nullptr; and of every size.
  const std::vector<uint64_t>* FontAtlases(uint32_t width, uint32_t height) const;
  std::vector<uint64_t> AllFontAtlases() const;
  void RememberContent(uint64_t guest_hash, uint64_t replacement_hash);

  // UI style: false = 360 (PS3 UI set unused), true = PS3.
  bool ps3_ui() const { return ps3_ui_.load(std::memory_order_relaxed); }
  void SetPs3Ui(bool ps3);
  // Changes whenever the style changes, so the texture cache can re-resolve its bindings.
  uint32_t style_generation() const { return style_generation_.load(std::memory_order_acquire); }
  bool SetActive(Set set) const {
    return set == Set::kUser || (set == Set::kKeyboard ? keyboard_on() : ps3_ui());
  }

  // Keyboard icons (Lightning Returns, keyboard mode): a generated image that stands in for the
  // given guest textures (the button icon sheet) while keyboard mode is on, ahead of the other
  // sets. Each call writes a new file, so cached images are never stale; empty = off.
  void SetKeyboard(const std::filesystem::path& image, const std::vector<uint64_t>& hashes,
                   uint32_t width, uint32_t height);
  bool keyboard_on() const { return keyboard_on_.load(std::memory_order_relaxed); }
  // Changes whenever the keyboard image changes or is turned on or off.
  uint32_t keyboard_generation() const { return keyboard_generation_.load(std::memory_order_acquire); }
  bool KeyboardSize(uint32_t width, uint32_t height) const;

  // Shader capture (LR_SHADER_CAPTURE=<file with one "<HASH> <w>x<h>" line per texture>): draws
  // that sample one of these textures record their shaders (TextureCache::LrCaptureDraw). Their
  // sizes are always hashed, with or without replacement files.
  bool capture_enabled() const { return !capture_hashes_.empty(); }
  bool CaptureHash(uint64_t hash) const { return capture_hashes_.count(hash) != 0; }

 private:
  Registry();
  void AddFolder(const std::filesystem::path& dir, Set set);
  std::filesystem::path root_;
  bool replace_enabled_ = true;
  bool has_user_ = false;
  bool dump_enabled_ = false;
  bool any_size_ = false;
  std::unordered_set<uint64_t> sizes_;  // width << 32 | height
  std::unordered_map<uint64_t, Entry> replacements_;
  std::unordered_map<uint64_t, std::vector<uint64_t>> sig_sizes_;  // width << 32 | height -> hashes
  std::unordered_map<uint64_t, std::vector<uint64_t>> font_sizes_;  // width << 32 | height -> hashes
  std::mutex content_mutex_;
  std::unordered_map<uint64_t, uint64_t> content_;
  std::mutex dump_mutex_;
  std::unordered_set<uint64_t> dumped_;
  std::atomic<bool> ps3_ui_{false};
  mutable std::mutex keyboard_mutex_;
  std::deque<Entry> keyboard_entries_;  // never shrinks: Find hands out pointers into it
  std::unordered_map<uint64_t, const Entry*> keyboard_;
  std::unordered_set<uint64_t> keyboard_sizes_;
  std::atomic<bool> keyboard_on_{false};
  std::atomic<uint32_t> keyboard_generation_{0};
  std::atomic<uint32_t> style_generation_{0};
  std::unordered_set<uint64_t> capture_hashes_;
  std::unordered_set<uint64_t> capture_sizes_;  // width << 32 | height
};

}  // namespace rex::graphics::texture_replacement
