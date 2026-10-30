#include "android.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

#include "install.h"

namespace trg::hub {
namespace fs = std::filesystem;

namespace {

std::string Utf8Of(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

// Unpacks one entry of a zip (an .apk is one) into `into`, with the system's tar (bsdtar reads zips).
bool UnzipEntry(const std::string& zip, const std::string& entry, const std::string& into) {
#if defined(_WIN32)
  wchar_t system_dir[MAX_PATH] = L"";
  GetSystemDirectoryW(system_dir, MAX_PATH);
  const std::wstring tar = std::wstring(system_dir) + L"\\tar.exe";
  std::wstring cmd = L"\"" + tar + L"\" -xf \"" + fs::u8path(zip).wstring() + L"\" -C \"" + fs::u8path(into).wstring() +
                     L"\" " + fs::u8path(entry).wstring();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return false;
  WaitForSingleObject(pi.hProcess, 60000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
#else
  const std::string cmd = "unzip -q -o '" + zip + "' '" + entry + "' -d '" + into + "' >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
#endif
}

std::vector<fs::path> SearchRoots() {
  std::vector<fs::path> roots;
#if defined(_WIN32)
  for (REFKNOWNFOLDERID id : {FOLDERID_Desktop, FOLDERID_Downloads, FOLDERID_Documents}) {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p))) roots.emplace_back(p);
    CoTaskMemFree(p);
  }
#else
  if (const char* home = std::getenv("HOME")) {
    roots.emplace_back(fs::path(home) / "Desktop");
    roots.emplace_back(fs::path(home) / "Downloads");
    roots.emplace_back(fs::path(home) / "Documents");
  }
#endif
  return roots;
}

bool EndsWithNoCase(const std::string& s, const std::string& end) {
  if (s.size() < end.size()) return false;
  return std::equal(end.rbegin(), end.rend(), s.rbegin(),
                    [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
}

}  // namespace

bool ApkHasPackage(const std::string& apk, const std::string& want) {
  std::error_code ec;
  const fs::path tmp = fs::temp_directory_path(ec) / ("trg_apk_" + std::to_string(std::hash<std::string>{}(apk)));
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  bool found = false;
  if (UnzipEntry(apk, "AndroidManifest.xml", Utf8Of(tmp))) {
    std::ifstream in(tmp / "AndroidManifest.xml", std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Binary XML keeps strings as UTF-16LE; a text manifest (rare) as plain bytes.
    std::string utf16;
    for (char c : want) {
      utf16 += c;
      utf16 += '\0';
    }
    found = data.find(utf16) != std::string::npos || data.find("\"" + want + "\"") != std::string::npos;
  }
  fs::remove_all(tmp, ec);
  return found;
}

std::string FindObbFor(const std::string& apk, const std::string& package) {
  std::error_code ec;
  const fs::path dir = fs::u8path(apk).parent_path();
  const std::string suffix = "." + package + ".obb";
  for (const fs::path& d : {dir, dir / "Android" / "obb" / package, dir / package}) {
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string name = Utf8Of(it->path().filename());
      if (name.rfind("main.", 0) == 0 && EndsWithNoCase(name, suffix)) return Utf8Of(it->path());
    }
    ec.clear();
  }
  return std::string();
}

ApkFinder::~ApkFinder() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
}

void ApkFinder::Start() {
  if (running_ || done_) return;
  if (worker_.joinable()) worker_.join();
  running_ = true;
  worker_ = std::thread([this] { Run(); });
}

void ApkFinder::Rescan() {
  if (running_) return;
  done_ = false;
  Start();
}

std::vector<FoundApk> ApkFinder::Found(const std::string& package) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<FoundApk> out;
  for (const auto& f : found_)
    if (f.first == package) out.push_back(f.second);
  return out;
}

void ApkFinder::Run() {
  std::vector<std::pair<std::string, FoundApk>> found;
  std::vector<std::string> packages;
  for (const GameInfo& g : Games())
    if (g.source == InstallSource::kApk && g.android_package) packages.push_back(g.android_package);
  for (const fs::path& root : SearchRoots()) {
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && !stop_; it.increment(ec)) {
      if (it.depth() > 3) {
        it.disable_recursion_pending();
        continue;
      }
      if (!it->is_regular_file(ec)) continue;
      const std::string path = Utf8Of(it->path());
      if (!EndsWithNoCase(path, ".apk")) continue;
      for (const std::string& pkg : packages) {
        if (!ApkHasPackage(path, pkg)) continue;
        FoundApk a;
        a.path = path;
        a.obb = FindObbFor(path, pkg);
        a.size = (long long)it->file_size(ec);
        found.push_back({pkg, a});
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    found_ = std::move(found);
  }
  done_ = true;
  running_ = false;
}

void StartApkInstall(const GameInfo& game, const std::string& apk, GameJobs& jobs) {
  if (jobs.task.running()) return;
  jobs.kind = GameJobs::Kind::kInstall;
  jobs.message.clear();
  jobs.message_is_error = false;
  const std::string title = game.title;
  const std::string package = game.android_package ? game.android_package : "";
  const bool needs_obb = game.needs_obb;
  const auto install = game.install_apk;
  ApkInstall a;
  a.apk = apk;
  a.dest = GameDir(game);
  jobs.task.Start("Looking at your copy of the game", [a, title, package, needs_obb, install](trg::Task& task) mutable {
    if (!install) return std::string("This game cannot be installed from an Android package yet.");
    if (!EndsWithNoCase(a.apk, ".apk") || !ApkHasPackage(a.apk, package))
      return "That is not the " + title + " Android package (" + package + "). Choose your " + title + " .apk file.";
    a.obb = FindObbFor(a.apk, package);
    if (needs_obb && a.obb.empty())
      return "Your " + title + " .apk was found, but not its expansion file. Put main.<number>." + package +
             ".obb in the same folder as the .apk, then try again.";
    std::error_code ec;
    fs::create_directories(fs::u8path(a.dest), ec);
    task.SetLabel("Installing " + title + " from your copy");
    return install(task, a);
  });
}

}  // namespace trg::hub
