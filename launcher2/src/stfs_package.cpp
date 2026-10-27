// Reader for Xbox 360 STFS packages (see stfs_package.h). From Trials HD Recompiled by snufflyyy (MIT, see
// THIRD_PARTY_NOTICES.md): src/installer/stfs_package.cpp, with Windows file writes.

#include "stfs_package.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace trg::stfs {

namespace {

constexpr uint32_t kBlockSize = 0x1000;
constexpr uint32_t kEndOfChain = 0xFFFFFF;

inline uint16_t read_be16(const uint8_t* p) {
  return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

inline uint32_t read_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

inline uint64_t read_be64(const uint8_t* p) {
  return (static_cast<uint64_t>(read_be32(p)) << 32) |
         static_cast<uint64_t>(read_be32(p + 4));
}

inline uint16_t read_le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_u24le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}

bool Fail(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}

std::FILE* OpenForWrite(const std::filesystem::path& path) {
#if defined(_WIN32)
  return _wfopen(path.c_str(), L"wb");
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

bool WriteFile(const std::filesystem::path& destination, const char* data, size_t size,
               std::string* error) {
  // Replace only after a complete write, preserving existing files on disk-full errors.
  std::filesystem::path temporary = destination;
  temporary += ".tmp";
  std::FILE* file = OpenForWrite(temporary);
  bool written = false;
  if (file) {
    written = size == 0 || std::fwrite(data, 1, size, file) == size;
    if (std::fclose(file) != 0) written = false;
  }
  std::error_code ec;
  if (written) std::filesystem::rename(temporary, destination, ec);
  if (!written || ec) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Fail(error, "Failed to write " + destination.string());
  }
  return true;
}

}  // namespace

struct StfsPackage::Impl {
  std::ifstream file;
};

StfsPackage::StfsPackage() : impl_(new Impl()) {}

StfsPackage::~StfsPackage() {
  Close();
  delete impl_;
}

void StfsPackage::Close() {
  if (impl_ && impl_->file.is_open()) {
    impl_->file.close();
  }
  cache_.clear();
  entries_.clear();
  licenses_.clear();
  file_size_ = 0;
}

uint64_t StfsPackage::total_file_bytes() const {
  uint64_t total = 0;
  for (const auto& entry : entries_) {
    if (!entry.is_dir) total += entry.size;
  }
  return total;
}

const StfsEntry* StfsPackage::FindEntry(std::string_view path) const {
  for (const auto& entry : entries_) {
    if (entry.path == path) return &entry;
  }
  return nullptr;
}

uint32_t StfsPackage::license_mask() const {
  uint32_t mask = 0;
  for (const auto& lic : licenses_) {
    mask |= lic.bits;
  }
  return mask;
}

bool StfsPackage::Open(const std::filesystem::path& path, std::string* error) {
  Close();
  path_ = path;
  impl_->file.open(path, std::ios::binary);
  if (!impl_->file.is_open()) {
    if (error) *error = "Failed to open file: " + path.string();
    return false;
  }

  auto fail = [&](std::string message) {
    Close();
    return Fail(error, std::move(message));
  };
  impl_->file.seekg(0, std::ios::end);
  auto file_size = impl_->file.tellg();
  if (file_size < 0) return fail("Failed to determine package size");
  file_size_ = static_cast<uint64_t>(file_size);
  std::vector<uint8_t> hdr(0x971A);
  if (!ReadAt(0, hdr.data(), hdr.size())) return fail("Failed to read header (too short)");

  std::string_view magic(reinterpret_cast<const char*>(hdr.data()), 4);
  if (magic != "LIVE" && magic != "PIRS" && magic != "CON ") {
    if (error) *error = "Not an STFS package (magic: " + std::string(magic) + ")";
    Close();
    return false;
  }

  header_size_ = read_be32(hdr.data() + 0x340);
  content_type_ = read_be32(hdr.data() + 0x344);
  volume_type_ = read_be32(hdr.data() + 0x3A9);
  if (volume_type_ != 0) {
    if (error) *error = "SVOD packages are not supported";
    Close();
    return false;
  }

  const uint8_t* vd = hdr.data() + 0x379;
  flags_ = vd[2];
  read_only_ = (flags_ & 1) != 0;
  root_active_ = (flags_ & 2) != 0;
  ft_block_count_ = read_le16(vd + 3);
  ft_block_ = read_u24le(vd + 5);
  total_blocks_ = read_be32(vd + 0x1C);
  title_id_ = read_be32(hdr.data() + 0x360);

  // Display name: 18 UTF-16 BE slots of 0x80 bytes, one per language; packages
  // made on a console carry only the creator's language (a German console
  // fills slot 4), so take the first non-empty one.
  display_name_.clear();
  for (size_t slot = 0; slot < 18 && display_name_.empty(); ++slot) {
    const size_t base = 0x411 + slot * 0x80;
    for (size_t i = 0; i < 0x80; i += 2) {
      uint16_t c = (static_cast<uint16_t>(hdr[base + i]) << 8) |
                   static_cast<uint16_t>(hdr[base + i + 1]);
      if (c == 0) break;
      if (c < 0x80) {
        display_name_ += static_cast<char>(c);
      } else if (c < 0x800) {
        display_name_ += static_cast<char>(0xC0 | (c >> 6));
        display_name_ += static_cast<char>(0x80 | (c & 0x3F));
      } else {
        display_name_ += static_cast<char>(0xE0 | (c >> 12));
        display_name_ += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        display_name_ += static_cast<char>(0x80 | (c & 0x3F));
      }
    }
  }

  // 16 license entries at 0x22C
  licenses_.clear();
  for (int i = 0; i < 16; ++i) {
    const uint8_t* lp = hdr.data() + 0x22C + 16 * i;
    uint64_t lid = read_be64(lp);
    uint32_t bits = read_be32(lp + 8);
    uint32_t lflags = read_be32(lp + 12);
    if (lid != 0) {
      licenses_.push_back({lid, bits, lflags});
    }
  }

  bpht_ = read_only_ ? 1 : 2;
  step0_ = 0xAA + bpht_;
  step1_ = 0x70E4 + (0xAA + 1) * bpht_;
  base_ = (uint64_t(header_size_) + kBlockSize - 1) & ~uint64_t(kBlockSize - 1);
  if (header_size_ < hdr.size() || base_ > file_size_ ||
      total_blocks_ > kEndOfChain || total_blocks_ > file_size_ / kBlockSize ||
      ft_block_count_ > total_blocks_) {
    return fail("Invalid package bounds");
  }

  // Read file listing
  entries_.clear();
  uint32_t blk = ft_block_;
  std::vector<uint8_t> block_buf(kBlockSize);
  std::unordered_set<uint32_t> seen;

  for (uint32_t i = 0; i < ft_block_count_; ++i) {
    if (blk >= total_blocks_ || !seen.insert(blk).second ||
        !ReadAt(BlockToOffset(blk), block_buf.data(), kBlockSize)) {
      return fail("Invalid or truncated file-table chain");
    }

    for (int m = 0; m < 0x40; ++m) {
      const uint8_t* e = block_buf.data() + m * 0x40;
      if (e[0] == 0) {
        break;
      }
      uint32_t nl = e[0x28] & 0x3F;
      if (nl == 0 || nl > 0x28) return fail("Invalid file name length");
      std::string name(reinterpret_cast<const char*>(e), nl);
      if (name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos ||
          name.find('\0') != std::string::npos) {
        return fail("Unsafe package file name");
      }
      bool is_dir = (e[0x28] & 0x80) != 0;
      uint32_t start = read_u24le(e + 0x2F);
      uint16_t parent = read_be16(e + 0x32);
      uint32_t size = read_be32(e + 0x34);

      StfsEntry entry;
      entry.name = std::move(name);
      entry.is_dir = is_dir;
      entry.start_block = start;
      entry.parent = parent;
      entry.size = size;
      entries_.push_back(std::move(entry));
    }

    if (!NextBlock(blk, &blk) || seen.count(blk) != 0 ||
        (blk == kEndOfChain && i + 1 < ft_block_count_)) {
      return fail("Invalid or truncated file-table chain");
    }
  }

  // Resolve each path once; gray nodes detect cycles without recursion.
  std::vector<uint8_t> state(entries_.size());
  std::unordered_set<std::string> paths;
  for (size_t i = 0; i < entries_.size(); ++i) {
    std::vector<size_t> chain;
    size_t index = i;
    while (index < entries_.size() && state[index] != 2) {
      if (state[index] == 1) return fail("Cyclic package directory parents");
      state[index] = 1;
      chain.push_back(index);
      auto parent = entries_[index].parent;
      if (parent != 0xFFFF && (parent >= entries_.size() || !entries_[parent].is_dir)) {
        return fail("Invalid package directory parent");
      }
      index = parent == 0xFFFF ? entries_.size() : parent;
    }
    std::string entry_path = index == entries_.size() ? "" : entries_[index].path;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      entry_path = entry_path.empty() ? entries_[*it].name : entry_path + '/' + entries_[*it].name;
      if (entry_path.size() > 4096 || !paths.insert(entry_path).second) {
        return fail("Duplicate or overlong package path");
      }
      entries_[*it].path = entry_path;
      state[*it] = 2;
    }
  }

  return true;
}

uint64_t StfsPackage::BlockToOffset(uint32_t idx) const {
  uint64_t base_lvl = 0xAA;
  uint64_t block = idx;
  for (int i = 0; i < 3; ++i) {
    block += ((idx + base_lvl) / base_lvl) * bpht_;
    if (idx < base_lvl) break;
    base_lvl *= 0xAA;
  }
  return base_ + (block << 12);
}

uint32_t StfsPackage::HashBlockNumber(uint32_t idx, int level) const {
  if (level == 0) {
    if (idx < 0xAA) return 0;
    uint32_t block = (idx / 0xAA) * step0_;
    block += ((idx / 0x70E4) + 1) * bpht_;
    return (idx < 0x70E4) ? block : (block + bpht_);
  }
  if (level == 1) {
    if (idx < 0x70E4) return step0_;
    return (idx / 0x70E4) * step1_ + bpht_;
  }
  return step1_;
}

uint64_t StfsPackage::HashOffset(uint32_t idx, int level) const {
  return base_ + (static_cast<uint64_t>(HashBlockNumber(idx, level)) << 12);
}

bool StfsPackage::ReadAt(uint64_t off, uint8_t* data, size_t size) {
  if (!impl_->file.is_open() || off > file_size_ || size > file_size_ - off) return false;
  impl_->file.seekg(static_cast<std::streamoff>(off), std::ios::beg);
  return bool(impl_->file.read(reinterpret_cast<char*>(data), size));
}

const uint8_t* StfsPackage::GetTable(uint64_t off) {
  auto it = cache_.find(off);
  if (it != cache_.end()) {
    return it->second.data();
  }
  std::vector<uint8_t> buf(kBlockSize);
  if (!ReadAt(off, buf.data(), kBlockSize)) return nullptr;
  auto res = cache_.emplace(off, std::move(buf));
  return res.first->second.data();
}

bool StfsPackage::NextBlock(uint32_t idx, uint32_t* next) {
  if (idx >= total_blocks_) return false;
  uint32_t sec = root_active_ ? kBlockSize : 0;
  if (read_only_) {
    sec = 0;
  } else {
    if (total_blocks_ > 0xAA) {
      if (total_blocks_ > 0x70E4) {
        const uint8_t* t2 = GetTable(HashOffset(idx, 2) + sec);
        if (!t2) return false;
        uint32_t record = (idx / 0x70E4) % 0xAA;
        uint8_t st = t2[record * 0x18 + 0x14];
        sec = (st & 0x40) ? kBlockSize : 0;
      }
      const uint8_t* t1 = GetTable(HashOffset(idx, 1) + sec);
      if (!t1) return false;
      uint32_t record = (idx / 0xAA) % 0xAA;
      uint8_t st = t1[record * 0x18 + 0x14];
      sec = (st & 0x40) ? kBlockSize : 0;
    }
  }
  const uint8_t* t0 = GetTable(HashOffset(idx, 0) + sec);
  if (!t0) return false;
  uint32_t record = idx % 0xAA;
  const uint8_t* e = t0 + record * 0x18;
  *next = (static_cast<uint32_t>(e[0x15]) << 16) |
          (static_cast<uint32_t>(e[0x16]) << 8) |
          static_cast<uint32_t>(e[0x17]);
  return *next == kEndOfChain || *next < total_blocks_;
}

bool StfsPackage::ReadFile(const StfsEntry& entry, std::vector<uint8_t>& out_data, std::string* error) {
  if (entry.size > file_size_) return Fail(error, "File size exceeds package size");
  out_data.resize(entry.size);
  uint32_t remaining = entry.size;
  uint32_t blk = entry.start_block;
  uint32_t offset = 0;
  std::unordered_set<uint32_t> seen;

  while (remaining > 0) {
    uint32_t n = std::min<uint32_t>(kBlockSize, remaining);
    if (blk >= total_blocks_ || !seen.insert(blk).second ||
        !ReadAt(BlockToOffset(blk), out_data.data() + offset, n)) {
      return Fail(error, "Invalid or truncated data chain for " + entry.path);
    }
    remaining -= n;
    offset += n;
    if (!NextBlock(blk, &blk) || seen.count(blk) != 0) {
      return Fail(error, "Invalid data chain for " + entry.path);
    }
  }
  return true;
}

bool StfsPackage::ExtractEntry(const StfsEntry& entry, const std::filesystem::path& destination,
                               const std::function<bool(uint64_t)>& on_bytes,
                               std::string* error) {
  if (entry.size > file_size_) return Fail(error, "File size exceeds package size");
  std::filesystem::path temporary = destination;
  temporary += ".tmp";
  std::FILE* file = OpenForWrite(temporary);
  if (!file) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Fail(error, "Failed to write " + destination.string());
  }
  // Blocks are read one at a time (the chain can jump) but written in 1 MiB
  // batches, which is also how often progress is reported.
  constexpr size_t kBatch = 256 * kBlockSize;
  std::vector<uint8_t> batch(kBatch);
  size_t batched = 0;
  uint32_t remaining = entry.size;
  uint32_t blk = entry.start_block;
  std::unordered_set<uint32_t> seen;
  bool ok = true;
  std::string message;
  auto flush = [&] {
    if (batched && std::fwrite(batch.data(), 1, batched, file) != batched) {
      message = "Failed to write " + destination.string();
      return false;
    }
    batched = 0;
    return true;
  };
  while (ok && remaining > 0) {
    uint32_t n = std::min<uint32_t>(kBlockSize, remaining);
    if (blk >= total_blocks_ || !seen.insert(blk).second ||
        !ReadAt(BlockToOffset(blk), batch.data() + batched, n)) {
      message = "Invalid or truncated data chain for " + entry.path;
      ok = false;
      break;
    }
    batched += n;
    remaining -= n;
    if (!NextBlock(blk, &blk) || seen.count(blk) != 0) {
      message = "Invalid data chain for " + entry.path;
      ok = false;
      break;
    }
    if (batched == kBatch || remaining == 0) {
      const uint64_t written = entry.size - remaining;
      ok = flush();
      if (ok && on_bytes && !on_bytes(written)) {
        message = "Cancelled";
        ok = false;
      }
    }
  }
  if (std::fclose(file) != 0 && ok) {
    message = "Failed to write " + destination.string();
    ok = false;
  }
  std::error_code ec;
  if (ok) std::filesystem::rename(temporary, destination, ec);
  if (!ok || ec) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Fail(error, ok ? "Failed to write " + destination.string() : message);
  }
  return true;
}

bool StfsPackage::ExtractFile(const StfsEntry& entry, const std::filesystem::path& destination,
                              std::string* error) {
  if (entry.is_dir) return Fail(error, entry.path + " is a directory");
  return ExtractEntry(entry, destination, nullptr, error);
}

bool StfsPackage::ExtractAll(const std::filesystem::path& out_dir,
                             bool write_license,
                             const ExtractProgress& progress,
                             std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) return Fail(error, "Failed to create " + out_dir.string() + ": " + ec.message());
  auto root = std::filesystem::canonical(out_dir, ec);
  if (ec) return Fail(error, "Invalid extraction directory: " + ec.message());
  // shortcut: caller owns this directory; use directory-relative no-follow I/O for shared targets.
  auto safe_path = [&](const std::string& relative) {
    auto path = root;
    for (const auto& part : std::filesystem::path(relative)) {
      path /= part;
      auto status = std::filesystem::symlink_status(path, ec);
      if (std::filesystem::is_symlink(status) ||
          (ec && ec != std::errc::no_such_file_or_directory)) return false;
    }
    return true;
  };
  for (const auto& entry : entries_) {
    if (!safe_path(entry.path)) return Fail(error, "Unsafe extraction path: " + entry.path);
  }
  bool license = write_license && content_type_ == 0x000D0000;
  if (license && !safe_path("package_license.toml")) return Fail(error, "Unsafe license path");

  const uint64_t bytes_total = total_file_bytes();
  uint64_t bytes_done = 0;
  for (const auto& entry : entries_) {
    if (progress && !progress(entry, bytes_done, bytes_total)) return Fail(error, "Cancelled");
    auto destination = root / entry.path;
    std::filesystem::create_directories(entry.is_dir ? destination : destination.parent_path(), ec);
    if (ec) return Fail(error, "Failed to create directory: " + ec.message());
    if (entry.is_dir) continue;
    auto on_bytes = [&](uint64_t written) {
      return !progress || progress(entry, bytes_done + written, bytes_total);
    };
    if (!ExtractEntry(entry, destination, on_bytes, error)) return false;
    bytes_done += entry.size;
  }

  if (license) {
    std::ostringstream text;
    text << "# Written from the package's license block when the game was installed.\n";
    text << "license_mask = 0x" << std::uppercase << std::hex << license_mask() << "\n";
    auto data = text.str();
    return WriteFile(root / "package_license.toml", data.data(), data.size(), error);
  }
  return true;
}

}  // namespace trg::stfs
