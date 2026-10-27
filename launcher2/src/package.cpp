#include "package.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <vector>

#include "stfs_package.h"

namespace trg::hub {
namespace fs = std::filesystem;

namespace {

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::string Utf8(const fs::path& p) {
  auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

constexpr uint32_t kContentTypeArcade = 0x000D0000;

}  // namespace

uint32_t ReadPackageTitleId(const std::string& utf8_file) {
  std::ifstream f(fs::u8path(utf8_file), std::ios::binary);
  std::array<uint8_t, 0x400> h{};
  if (!f.read(reinterpret_cast<char*>(h.data()), h.size())) return 0;
  const bool magic = (h[0] == 'L' && h[1] == 'I' && h[2] == 'V' && h[3] == 'E') ||
                     (h[0] == 'P' && h[1] == 'I' && h[2] == 'R' && h[3] == 'S') ||
                     (h[0] == 'C' && h[1] == 'O' && h[2] == 'N' && h[3] == ' ');
  if (!magic || Be32(&h[0x344]) != kContentTypeArcade) return 0;
  return Be32(&h[0x360]);
}

uint32_t ReadXexTitleId(const std::string& utf8_xex) {
  std::ifstream f(fs::u8path(utf8_xex), std::ios::binary);
  std::vector<uint8_t> h(0x1000);
  f.read(reinterpret_cast<char*>(h.data()), std::streamsize(h.size()));
  const size_t got = size_t(f.gcount());
  if (got < 0x18 || h[0] != 'X' || h[1] != 'E' || h[2] != 'X' || h[3] != '2') return 0;
  const uint32_t count = Be32(&h[0x14]);
  for (uint32_t i = 0; i < count && 0x20 + i * 8 <= got; ++i) {
    const uint8_t* e = &h[0x18 + i * 8];
    if (Be32(e) != 0x00040006) continue;  // execution info: media ID, version, base version, title ID
    const uint32_t at = Be32(e + 4);
    if (at + 16 > got) return 0;
    return Be32(&h[at + 12]);
  }
  return 0;
}

GameSource FindGameSource(const std::string& utf8_path, uint32_t want) {
  GameSource other;  // another game's copy, when that is all there is
  std::error_code ec;
  const fs::path path = fs::u8path(utf8_path);
  auto consider = [&](const fs::path& p, bool extracted, uint32_t id) -> bool {
    if (id == 0) return false;
    if (id == want) {
      other = {Utf8(p), extracted, id};
      return true;
    }
    if (other.title_id == 0) other = {Utf8(p), extracted, id};
    return false;
  };
  if (fs::is_regular_file(path, ec)) {
    if (Lower(Utf8(path.filename())) == "default.xex")
      consider(path.parent_path(), true, ReadXexTitleId(utf8_path));
    else
      consider(path, false, ReadPackageTitleId(utf8_path));
    return other;
  }
  if (!fs::is_directory(path, ec)) return {};
  // The folder itself extracted, then a package or extracted folder up to four folders down.
  if (fs::exists(path / "default.xex", ec) && consider(path, true, ReadXexTitleId(Utf8(path / "default.xex"))))
    return other;
  fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (it.depth() > 4) {
      it.disable_recursion_pending();
      continue;
    }
    std::error_code ec2;
    if (!it->is_regular_file(ec2)) continue;
    const fs::path& p = it->path();
    const std::string name = Lower(Utf8(p.filename()));
    bool match = false;
    if (name == "default.xex")
      match = consider(p.parent_path(), true, ReadXexTitleId(Utf8(p)));
    else if (it->file_size(ec2) >= 0xA000 && p.extension().empty())
      match = consider(p, false, ReadPackageTitleId(Utf8(p)));
    if (match) return other;
  }
  return other.title_id == want ? other : GameSource{"", false, other.title_id};
}

std::string InstallGameSource(trg::Task& task, const GameSource& source, const std::string& out_dir) {
  std::error_code ec;
  const fs::path out = fs::u8path(out_dir);
  fs::create_directories(out, ec);
  if (source.extracted) {
    // Copy the extracted folder, counting bytes for the progress bar.
    const fs::path from = fs::u8path(source.path);
    long long total = 0, done = 0;
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      std::error_code ec2;
      if (it->is_regular_file(ec2)) total += static_cast<long long>(it->file_size(ec2));
    }
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (task.cancelled()) return "Cancelled.";
      const fs::path to = out / fs::relative(it->path(), from, ec);
      std::error_code ec2;
      if (it->is_directory(ec2)) {
        fs::create_directories(to, ec2);
        continue;
      }
      if (!fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, ec2))
        return "Could not copy " + Utf8(it->path().filename()) + ": " + ec2.message();
      done += static_cast<long long>(it->file_size(ec2));
      task.Progress(done, total);
    }
    return ec ? "Could not read the folder: " + ec.message() : std::string();
  }
  stfs::StfsPackage pkg;
  std::string error;
  if (!pkg.Open(fs::u8path(source.path), &error)) return "The package could not be read: " + error;
  const bool ok = pkg.ExtractAll(out, true,
                                 [&task](const stfs::StfsEntry&, uint64_t done, uint64_t total) {
                                   task.Progress(static_cast<long long>(done), static_cast<long long>(total));
                                   return !task.cancelled();
                                 },
                                 &error);
  if (task.cancelled()) return "Cancelled.";
  return ok ? std::string() : "The package could not be extracted: " + error;
}

}  // namespace trg::hub
