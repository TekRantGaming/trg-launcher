#include "package_finder.h"

#include <algorithm>
#include <cctype>
#include <filesystem>


#include "games.h"
#include "package.h"

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

namespace trg::hub {
namespace fs = std::filesystem;

namespace {

std::string Utf8(const fs::path& p) {
  auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

bool Skip(const fs::path& dir) {
  std::string n = Utf8(dir.filename());
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return n.empty() || n[0] == '$' || n[0] == '.' || n == "windows" || n == "program files" ||
         n == "program files (x86)" || n == "programdata" || n == "appdata" || n == "system volume information" ||
         n == "recovery" || n == "node_modules";
}

}  // namespace

PackageFinder::~PackageFinder() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
}

void PackageFinder::Start() {
  if (running_ || done_) return;
  if (worker_.joinable()) worker_.join();
  running_ = true;
  worker_ = std::thread([this] { Run(); });
}

void PackageFinder::Rescan() {
  if (running_) return;
  done_ = false;
  Start();
}

std::vector<FoundPackage> PackageFinder::Found(uint32_t title_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<FoundPackage> out;
  for (const FoundPackage& d : found_)
    if (d.title_id == title_id) out.push_back(d);
  return out;
}

void PackageFinder::Run() {
  std::vector<std::pair<fs::path, int>> roots;  // folder, depth to search
#if defined(_WIN32)
  const KNOWNFOLDERID* known[] = {&FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents};
  for (const KNOWNFOLDERID* id : known) {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, nullptr, &p))) roots.push_back({fs::path(p), 5});
    CoTaskMemFree(p);
  }
  const DWORD drives = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (!(drives & (1u << i))) continue;
    wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', 0};
    if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
    roots.push_back({fs::path(root), 5});
  }
#else
  // The usual folders in the home folder, then removable drives and other mounted disks.
  const fs::path home = getenv("HOME") ? getenv("HOME") : "";
  if (!home.empty()) {
    for (const char* d : {"Downloads", "Desktop", "Documents"}) roots.push_back({home / d, 5});
    roots.push_back({home, 1});
  }
  if (const char* user = getenv("USER")) roots.push_back({fs::path("/run/media") / user, 5});
  roots.push_back({"/media", 5});
  roots.push_back({"/mnt", 4});
#endif
  std::vector<FoundPackage> found;
  const std::string own = LauncherDir() + "/games/";
  std::vector<std::string> seen;
  for (const auto& [root, depth] : roots) {
    if (stop_) break;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && !stop_; it.increment(ec)) {
      if (it.depth() > depth) {
        it.disable_recursion_pending();
        continue;
      }
      const fs::directory_entry& e = *it;
      std::error_code ec2;
      if (e.is_directory(ec2)) {
        if (Skip(e.path()) || it.depth() >= depth) it.disable_recursion_pending();
        continue;
      }
      // A package is the one file in a 000D0000 folder; an extracted game has default.xex. The games this
      // launcher installed itself are skipped.
      std::string parent = Utf8(e.path().parent_path().filename());
      std::transform(parent.begin(), parent.end(), parent.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      std::string name = Utf8(e.path().filename());
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      const bool xex = name == "default.xex";
      if (!xex && (parent != "000d0000" || e.path().has_extension())) continue;
      const std::string path = xex ? Utf8(e.path().parent_path()) : Utf8(e.path());
      if (path.rfind(own, 0) == 0) continue;
      if (std::find(seen.begin(), seen.end(), path) != seen.end()) continue;
      seen.push_back(path);
      const uint32_t id = xex ? ReadXexTitleId(Utf8(e.path())) : ReadPackageTitleId(path);
      for (const GameInfo& g : Games()) {
        if (g.title_id == id) found.push_back({path, id, static_cast<long long>(e.file_size(ec2)), xex});
      }
      std::lock_guard<std::mutex> lock(mu_);
      found_ = found;
    }
  }
  std::lock_guard<std::mutex> lock(mu_);
  found_ = found;
  running_ = false;
  done_ = true;
}

}  // namespace trg::hub
