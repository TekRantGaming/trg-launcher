// Reader for Xbox 360 STFS (LIVE/PIRS/CON) packages: the player's game packages, extracted by the install
// (install.cpp). From Trials HD Recompiled by snufflyyy (MIT, see THIRD_PARTY_NOTICES.md): src/installer/stfs_package.h.
// Block/hash math mirrors the SDK's src/filesystem/devices/stfs_container_device.cpp.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace trg::stfs {

struct StfsLicense {
  uint64_t id = 0;
  uint32_t bits = 0;
  uint32_t flags = 0;
};

struct StfsEntry {
  std::string name;
  std::string path;
  bool is_dir = false;
  uint32_t start_block = 0;
  uint16_t parent = 0xFFFF;
  uint32_t size = 0;
};

// ExtractAll progress: the entry being written and the file bytes written so far
// out of all file bytes in the package. Return false to cancel the extraction.
using ExtractProgress =
    std::function<bool(const StfsEntry& entry, uint64_t bytes_done, uint64_t bytes_total)>;

class StfsPackage {
 public:
  StfsPackage();
  ~StfsPackage();

  bool Open(const std::filesystem::path& path, std::string* error = nullptr);
  void Close();

  uint32_t header_size() const { return header_size_; }
  uint32_t content_type() const { return content_type_; }
  uint32_t volume_type() const { return volume_type_; }
  uint32_t title_id() const { return title_id_; }
  const std::string& display_name() const { return display_name_; }
  bool read_only() const { return read_only_; }
  uint32_t total_blocks() const { return total_blocks_; }
  const std::vector<StfsLicense>& licenses() const { return licenses_; }
  uint32_t license_mask() const;

  const std::vector<StfsEntry>& entries() const { return entries_; }
  // Sum of all file sizes (what ExtractAll writes).
  uint64_t total_file_bytes() const;
  const StfsEntry* FindEntry(std::string_view path) const;

  bool ReadFile(const StfsEntry& entry, std::vector<uint8_t>& out_data, std::string* error = nullptr);
  // One file to `destination` (its folder must exist), streamed and renamed
  // into place when complete, like ExtractAll.
  bool ExtractFile(const StfsEntry& entry, const std::filesystem::path& destination,
                   std::string* error = nullptr);
  // Files are streamed to a temporary name and renamed when complete, so a
  // failed or cancelled extraction never leaves a truncated file in place.
  bool ExtractAll(const std::filesystem::path& out_dir,
                  bool write_license = true,
                  const ExtractProgress& progress = nullptr,
                  std::string* error = nullptr);

 private:
  uint64_t BlockToOffset(uint32_t idx) const;
  uint32_t HashBlockNumber(uint32_t idx, int level) const;
  uint64_t HashOffset(uint32_t idx, int level) const;
  const uint8_t* GetTable(uint64_t off);
  bool NextBlock(uint32_t idx, uint32_t* next);
  bool ReadAt(uint64_t off, uint8_t* data, size_t size);
  bool ExtractEntry(const StfsEntry& entry, const std::filesystem::path& destination,
                    const std::function<bool(uint64_t)>& on_bytes, std::string* error);

  std::filesystem::path path_;
  struct Impl;
  Impl* impl_ = nullptr;

  uint32_t header_size_ = 0;
  uint32_t content_type_ = 0;
  uint32_t volume_type_ = 0;
  uint8_t flags_ = 0;
  bool read_only_ = false;
  bool root_active_ = false;
  uint16_t ft_block_count_ = 0;
  uint32_t ft_block_ = 0;
  uint32_t total_blocks_ = 0;
  uint32_t title_id_ = 0;
  std::string display_name_;
  std::vector<StfsLicense> licenses_;
  uint32_t bpht_ = 1;
  uint32_t step0_ = 0;
  uint32_t step1_ = 0;
  uint64_t base_ = 0;
  uint64_t file_size_ = 0;
  std::vector<StfsEntry> entries_;
  std::unordered_map<uint64_t, std::vector<uint8_t>> cache_;
};

}  // namespace trg::stfs
