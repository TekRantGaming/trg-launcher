// Updates from GitHub releases (updater.h): versions, the GitHub answer, the
// check, install and first start check jobs, restarting, and the Updates rows.
#include "trg/updater.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "trg/download.h"
#include "trg/launcher.h"
#include "utf8_path.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace trg {
namespace {

namespace fs = std::filesystem;
namespace up = trg::update;
using detail::ToUtf8;
using detail::Utf8Path;

// Player facing messages.
constexpr const char* kCheckFailed = "Could not check for updates. Check your internet connection and try again.";
constexpr const char* kDamaged = "The download did not pass its check, so nothing was changed. Try again later.";
constexpr const char* kUnpackFailed = "Could not unpack the update, so nothing was changed.";
constexpr const char* kNoProgram = "The update did not contain the game's program, so nothing was changed.";
constexpr const char* kListDamaged = "The update's file list is damaged, so nothing was changed.";
constexpr const char* kSaveFailed = "Could not save the download. Is the disk full?";
constexpr const char* kWriteFailed =
    "Could not write the update into the game folder, so nothing was changed. Is it read only?";
constexpr const char* kInstallingLabel = "Installing the files";
constexpr uint64_t kCheapHashLimit = 4ull << 20;  // the first start check hashes files up to this size

// ------------------------------------------------------------- versions ---
struct Version {
  std::vector<long long> numbers;
  std::vector<std::string> pre;  // "beta.10" -> {"beta", "10"}; empty for a release
};

Version ParseVersion(std::string_view v) {
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.remove_prefix(1);
  if (const size_t plus = v.find('+'); plus != std::string_view::npos) v = v.substr(0, plus);  // build metadata
  Version out;
  const size_t dash = v.find('-');
  const std::string_view main = v.substr(0, dash);
  for (size_t at = 0; at <= main.size();) {
    const size_t end = std::min(main.find('.', at), main.size());
    out.numbers.push_back(std::atoll(std::string(main.substr(at, end - at)).c_str()));
    at = end + 1;
  }
  while (out.numbers.size() > 1 && out.numbers.back() == 0) out.numbers.pop_back();  // 1.4 == 1.4.0
  if (dash != std::string_view::npos) {
    // Runs of letters and of digits: "beta.10", "beta10" and "beta-10" all read as {"beta", "10"}.
    std::string run;
    auto flush = [&] {
      if (!run.empty()) out.pre.push_back(run), run.clear();
    };
    for (char c : v.substr(dash + 1)) {
      const bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
      if (!std::isalnum(static_cast<unsigned char>(c))) {
        flush();
        continue;
      }
      if (!run.empty() && (std::isdigit(static_cast<unsigned char>(run.back())) != 0) != digit) flush();
      run += char(std::tolower(static_cast<unsigned char>(c)));
    }
    flush();
    if (out.pre.empty()) out.pre.push_back("0");
  }
  return out;
}

int CompareNumbers(const Version& a, const Version& b) {
  for (size_t i = 0; i < std::max(a.numbers.size(), b.numbers.size()); ++i) {
    const long long x = i < a.numbers.size() ? a.numbers[i] : 0, y = i < b.numbers.size() ? b.numbers[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string GenericUtf8(const fs::path& p) {
  const auto s = p.generic_u8string();  // std::string in C++17, std::u8string in C++20
  return std::string(s.begin(), s.end());
}

bool EndsWith(std::string_view s, std::string_view tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// ----------------------------------------------------------------- JSON ---
// Just enough JSON for GitHub's release answers.
struct Json {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject } type = Type::kNull;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;

  const Json* Get(std::string_view key) const {
    for (const auto& [k, v] : members)
      if (k == key) return &v;
    return nullptr;
  }
  std::string Str(std::string_view key) const {
    const Json* v = Get(key);
    return v && v->type == Type::kString ? v->string : std::string();
  }
  bool Bool(std::string_view key) const {
    const Json* v = Get(key);
    return v && v->type == Type::kBool && v->boolean;
  }
  uint64_t Uint(std::string_view key) const {
    const Json* v = Get(key);
    return v && v->type == Type::kNumber && v->number > 0 ? uint64_t(v->number) : 0;
  }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : s_(text) {}
  bool Parse(Json& out) {
    if (!Value(out, 0)) return false;
    Space();
    return at_ == s_.size();
  }

 private:
  void Space() {
    while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\r' || s_[at_] == '\n')) ++at_;
  }
  bool Literal(std::string_view word) {
    if (s_.substr(at_, word.size()) != word) return false;
    at_ += word.size();
    return true;
  }
  static void Utf8Append(std::string& out, uint32_t c) {
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
  }
  bool Hex4(uint32_t& v) {
    if (at_ + 4 > s_.size()) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[at_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool String(std::string& out) {
    if (at_ >= s_.size() || s_[at_] != '"') return false;
    ++at_;
    while (at_ < s_.size()) {
      const char c = s_[at_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (at_ >= s_.size()) return false;
      switch (const char e = s_[at_++]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          uint32_t c1 = 0;
          if (!Hex4(c1)) return false;
          if (c1 >= 0xD800 && c1 < 0xDC00 && Literal("\\u")) {
            uint32_t c2 = 0;
            if (!Hex4(c2)) return false;
            c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
          }
          Utf8Append(out, c1);
          break;
        }
        default: out += e; break;  // \" \\ \/
      }
    }
    return false;
  }
  bool Value(Json& v, int depth) {
    if (depth > 64) return false;
    Space();
    if (at_ >= s_.size()) return false;
    const char c = s_[at_];
    if (c == '{') {
      v.type = Json::Type::kObject;
      ++at_;
      Space();
      if (at_ < s_.size() && s_[at_] == '}') return ++at_, true;
      for (;;) {
        Space();
        std::string key;
        if (!String(key)) return false;
        Space();
        if (at_ >= s_.size() || s_[at_++] != ':') return false;
        Json item;
        if (!Value(item, depth + 1)) return false;
        v.members.emplace_back(std::move(key), std::move(item));
        Space();
        if (at_ < s_.size() && s_[at_] == ',') {
          ++at_;
          continue;
        }
        return at_ < s_.size() && s_[at_++] == '}';
      }
    }
    if (c == '[') {
      v.type = Json::Type::kArray;
      ++at_;
      Space();
      if (at_ < s_.size() && s_[at_] == ']') return ++at_, true;
      for (;;) {
        Json item;
        if (!Value(item, depth + 1)) return false;
        v.items.push_back(std::move(item));
        Space();
        if (at_ < s_.size() && s_[at_] == ',') {
          ++at_;
          continue;
        }
        return at_ < s_.size() && s_[at_++] == ']';
      }
    }
    if (c == '"') {
      v.type = Json::Type::kString;
      return String(v.string);
    }
    if (Literal("true")) return v.type = Json::Type::kBool, v.boolean = true, true;
    if (Literal("false")) return v.type = Json::Type::kBool, true;
    if (Literal("null")) return true;
    const char* begin = s_.data() + at_;
    char* end = nullptr;
    const std::string number(begin, std::min<size_t>(s_.size() - at_, 32));
    v.number = std::strtod(number.c_str(), &end);
    if (end == number.c_str()) return false;
    v.type = Json::Type::kNumber;
    at_ += size_t(end - number.c_str());
    return true;
  }

  std::string_view s_;
  size_t at_ = 0;
};

// ---------------------------------------------------------------- setup ---
void Say(const UpdaterConfig& c, const std::string& line) {
  if (c.log) c.log("update: " + line);
}

std::string Get(const UpdaterConfig& c, const std::string& url, std::string& out, Task* task) {
  return c.http ? c.http(url, out, task) : HttpGet(url, out, task);
}

fs::path WorkDir(const UpdaterConfig& c) {
  if (!c.work_dir.empty()) return Utf8Path(c.work_dir);
  std::string name = c.stable_repo.substr(c.stable_repo.find('/') + 1);
  for (char& ch : name)
    if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
  std::error_code ec;
  return fs::temp_directory_path(ec) / Utf8Path("trg_update_" + name);
}

// The protected paths, plus the work folder and state file when they are inside the program folder.
up::ProtectedPaths MakeKeep(const UpdaterConfig& c) {
  std::vector<std::string> entries = c.protected_paths;
  entries.push_back(ToUtf8(fs::absolute(WorkDir(c))));
  if (!c.state_file.empty()) entries.push_back(ToUtf8(fs::absolute(Utf8Path(c.state_file))));
  return up::ProtectedPaths(entries, c.program_dir);
}

up::Log LogTo(const UpdaterConfig& c) {
  return [&c](const std::string& line) { Say(c, line); };
}

std::string ReadText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

#if defined(_WIN32)
std::wstring Widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
  return w;
}
#else
std::string ShellQuote(const std::string& s) {
  std::string q = "'";
  for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return q + "'";
}
#endif

std::string DefaultUnpack(const std::string& zip, const std::string& into) {
#if defined(_WIN32)
  wchar_t system_dir[MAX_PATH] = L"";
  GetSystemDirectoryW(system_dir, MAX_PATH);
  const std::wstring tar = std::wstring(system_dir) + L"\\tar.exe";
  std::wstring cmd = L"\"" + tar + L"\" -xf \"" + Widen(zip) + L"\" -C \"" + Widen(into) + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return kUnpackFailed;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0 ? "" : kUnpackFailed;
#else
  if (std::system(("unzip -q -o " + ShellQuote(zip) + " -d " + ShellQuote(into) + " >/dev/null 2>&1").c_str()) == 0)
    return "";
  if (std::system(("bsdtar -xf " + ShellQuote(zip) + " -C " + ShellQuote(into) + " >/dev/null 2>&1").c_str()) == 0)
    return "";
  return "Could not unpack the update: install unzip with your package manager.";
#endif
}

// The unpacked folder that is the program folder: the one nearest the top holding `marker`, else (no
// program_name) the only top folder, else the unpacked folder itself.
fs::path FindRoot(const fs::path& files, const UpdaterConfig& c) {
  const std::string marker = Lower(c.program_name.empty() ? c.files_list : c.program_name);
  fs::path root;
  size_t depth = SIZE_MAX;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(files, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (Lower(ToUtf8(it->path().filename())) != marker || !it->is_regular_file(ec)) continue;
    const size_t d = size_t(std::distance(it->path().begin(), it->path().end()));
    if (d < depth) depth = d, root = it->path().parent_path();
  }
  if (!root.empty() || !c.program_name.empty()) return root;
  std::vector<fs::path> top;
  for (auto& e : fs::directory_iterator(files, ec)) top.push_back(e.path());
  return top.size() == 1 && fs::is_directory(top[0], ec) ? top[0] : files;
}

// The hash the release's SHA256SUMS.txt gives for its asset: "" when it has none (then only the size is
// checked); nullopt when the download failed or the file has no line for it.
std::optional<std::string> ExpectedHash(Task& task, const UpdaterConfig& c, const Release& r, std::string& why) {
  if (r.sums_url.empty()) {
    Say(c, "the release has no SHA256SUMS.txt: only its size is checked");
    return std::string();
  }
  std::string sums;
  if (std::string error = Get(c, r.sums_url, sums, &task); !error.empty()) return why = error, std::nullopt;
  std::string want = up::SumFor(sums, r.asset_name);
  if (want.empty()) {
    Say(c, "SHA256SUMS.txt has no line for " + r.asset_name);
    return why = kDamaged, std::nullopt;
  }
  return want;
}

// Downloads (or reuses a checked copy from an earlier try), checks the asset and returns it in `data`.
std::string DownloadChecked(Task& task, const UpdaterConfig& c, const Release& r, const fs::path& keep_copy,
                            std::string& data) {
  std::string why;
  task.SetLabel("Downloading version " + r.version);
  const auto want = ExpectedHash(task, c, r, why);
  if (!want) return why;
  std::error_code ec;
  if (!keep_copy.empty() && !want->empty() && fs::is_regular_file(keep_copy, ec) &&
      (r.asset_size == 0 || fs::file_size(keep_copy, ec) == r.asset_size) &&
      up::FileSha256(ToUtf8(keep_copy)) == *want) {
    Say(c, "using the checked download from an earlier try");
    data = ReadText(keep_copy);
    return "";
  }
  if (std::string error = Get(c, r.asset_url, data, &task); !error.empty()) return error;
  task.SetLabel("Checking the download");
  task.Progress(0, -1);
  if (r.asset_size && data.size() != r.asset_size) {
    Say(c, "the download is " + std::to_string(data.size()) + " bytes, the release lists " +
               std::to_string(r.asset_size));
    return kDamaged;
  }
  if (!want->empty() && up::Sha256Hex(data.data(), data.size()) != *want) {
    Say(c, "the download's sha256 does not match SHA256SUMS.txt");
    return kDamaged;
  }
  return "";
}

// Downloads, checks and unpacks a release zip into `work`; `root` is the unpacked program folder. A checked zip
// stays in `work` so a retry does not download it again.
std::string FetchZip(Task& task, const UpdaterConfig& c, const Release& r, const fs::path& work, fs::path& root) {
  std::error_code ec;
  const fs::path files = work / "files", zip = work / "update.zip";
  fs::remove_all(files, ec);
  fs::create_directories(files, ec);
  std::string data;
  if (std::string error = DownloadChecked(task, c, r, zip, data); !error.empty()) return error;
  {
    std::ofstream out(zip, std::ios::binary | std::ios::trunc);
    if (!out.write(data.data(), std::streamsize(data.size()))) {
      out.close();
      fs::remove(zip, ec);
      return kSaveFailed;
    }
  }
  data = std::string();
  task.SetLabel("Unpacking the update");
  if (std::string error = c.unpack ? c.unpack(ToUtf8(zip), ToUtf8(files)) : DefaultUnpack(ToUtf8(zip), ToUtf8(files));
      !error.empty())
    return error;
  root = FindRoot(files, c);
  if (root.empty()) return kNoProgram;
  return "";
}

std::string InstallMessage(const up::InstallResult& r) {
  switch (r.outcome) {
    case up::Outcome::kOk: return {};
    case up::Outcome::kNoSource: return kDamaged;
    case up::Outcome::kWrite: return kWriteFailed;
    case up::Outcome::kInUse:
      return r.path + " is in use by another program, so the update was undone and nothing was changed. Close that "
                      "program and try again.";
  }
  return kWriteFailed;
}

std::string InstallAppImage(Task& task, const UpdaterConfig& c, const Release& r, InstallReport* out) {
  const char* appimage = std::getenv("APPIMAGE");
  if (!appimage || !*appimage)
    return "This copy is not running as an AppImage, so it cannot update itself. Download the new version from the "
           "release page.";
  std::string data;
  if (std::string error = DownloadChecked(task, c, r, {}, data); !error.empty()) return error;
  if (data.size() < 4 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0) return kDamaged;
  if (task.cancelled()) return "Cancelled.";
  task.SetLabel(kInstallingLabel);
  const fs::path target = Utf8Path(appimage);
  fs::path tmp = target;
  tmp += ".new";
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f.write(data.data(), std::streamsize(data.size())))
      return fs::remove(tmp, ec), "Could not save the update beside the AppImage. Is its folder read only?";
  }
  fs::permissions(tmp,
                  fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                      fs::perms::others_exec,
                  ec);
  fs::rename(tmp, target, ec);  // allowed while it runs; the restart starts the new one
  if (ec) return fs::remove(tmp, ec), "Could not replace the AppImage with the new version.";
  if (out) {
    out->installed = true;
    out->files = 1;
    out->message = "Version " + r.version + " is installed. Restart to use it.";
  }
  return "";
}

}  // namespace

// ------------------------------------------------------------- versions ---
int CompareVersions(std::string_view a, std::string_view b) {
  const Version va = ParseVersion(a), vb = ParseVersion(b);
  if (const int n = CompareNumbers(va, vb)) return n;
  if (va.pre.empty() || vb.pre.empty()) return va.pre.empty() == vb.pre.empty() ? 0 : va.pre.empty() ? 1 : -1;
  for (size_t i = 0; i < std::max(va.pre.size(), vb.pre.size()); ++i) {
    if (i >= va.pre.size()) return -1;  // beta < beta.1
    if (i >= vb.pre.size()) return 1;
    const std::string &x = va.pre[i], &y = vb.pre[i];
    const bool nx = std::isdigit(static_cast<unsigned char>(x[0])) != 0;
    const bool ny = std::isdigit(static_cast<unsigned char>(y[0])) != 0;
    if (nx && ny) {
      const long long ix = std::atoll(x.c_str()), iy = std::atoll(y.c_str());
      if (ix != iy) return ix < iy ? -1 : 1;
    } else if (nx != ny) {
      return nx ? -1 : 1;  // numbers sort before words, as in semver
    } else if (x != y) {
      return x < y ? -1 : 1;
    }
  }
  return 0;
}

bool IsPrerelease(std::string_view version) { return !ParseVersion(version).pre.empty(); }

bool MatchesPattern(std::string_view name, std::string_view pattern) {
  if (pattern.empty()) return false;
  // Classic '*' matching with one backtrack point.
  size_t n = 0, p = 0, star = std::string_view::npos, mark = 0;
  while (n < name.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = n;
    } else if (p < pattern.size() && pattern[p] == name[n]) {
      ++p, ++n;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      n = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

std::optional<Release> PickUpdate(std::string_view channel, bool switching, std::string_view current,
                                  const std::optional<Release>& stable, const std::optional<Release>& beta) {
  auto differs = [&](const Release& r) { return CompareVersions(r.version, current) != 0; };
  if (channel != "beta") {
    if (stable && (switching ? differs(*stable) : IsNewerVersion(stable->version, current))) return stable;
    return std::nullopt;
  }
  if (switching) {
    if (beta && differs(*beta)) return beta;
    return std::nullopt;
  }
  std::optional<Release> pick;
  if (beta && IsNewerVersion(beta->version, current)) pick = beta;
  // A stable release only when its x.y.z is at least the running beta's: its own final release, not a hotfix
  // of an older line.
  if (stable && IsNewerVersion(stable->version, current) &&
      CompareNumbers(ParseVersion(stable->version), ParseVersion(current)) >= 0 &&
      (!pick || IsNewerVersion(stable->version, pick->version)))
    pick = stable;
  return pick;
}

std::optional<Release> ParseReleases(const std::string& json, const std::string& asset_pattern,
                                     const std::string& tag_prefix, bool allow_prereleases) {
  Json root;
  if (!JsonParser(json).Parse(root)) return std::nullopt;
  std::vector<const Json*> list;
  if (root.type == Json::Type::kArray)
    for (const Json& r : root.items) list.push_back(&r);
  else if (root.type == Json::Type::kObject)
    list.push_back(&root);
  std::optional<Release> best;
  for (const Json* r : list) {
    if (r->type != Json::Type::kObject || r->Bool("draft")) continue;
    std::string version = r->Str("tag_name");
    if (!tag_prefix.empty() && version.rfind(tag_prefix, 0) == 0) version.erase(0, tag_prefix.size());
    if (version.empty() || !std::isdigit(static_cast<unsigned char>(version[0]))) continue;
    if (!allow_prereleases && (r->Bool("prerelease") || IsPrerelease(version))) continue;
    Release rel;
    rel.version = version;
    rel.page_url = r->Str("html_url");
    if (const Json* assets = r->Get("assets"); assets && assets->type == Json::Type::kArray)
      for (const Json& a : assets->items) {
        const std::string name = a.Str("name");
        if (rel.asset_url.empty() && MatchesPattern(name, asset_pattern)) {
          rel.asset_name = name;
          rel.asset_url = a.Str("browser_download_url");
          rel.asset_size = a.Uint("size");
        }
        if (name == "SHA256SUMS.txt") rel.sums_url = a.Str("browser_download_url");
      }
    if (rel.asset_url.empty()) continue;
    if (!best || IsNewerVersion(rel.version, best->version)) best = rel;
  }
  return best;
}

// ----------------------------------------------------------------- jobs ---
std::string CheckForUpdate(Task& task, const UpdaterConfig& c, std::string channel, bool switching, UpdateCheck* out) {
  task.SetLabel("Checking for updates");
  if (channel != "beta" || c.beta_repo.empty()) channel = "stable";
  // Stable is read on both channels (a beta's final release is a stable one); beta only on beta.
  auto latest = [&](const std::string& repo, bool beta, bool& answered) -> std::optional<Release> {
    std::string json;
    const std::string error = Get(c, c.api_base + "/repos/" + repo + "/releases?per_page=20", json, &task);
    answered = error.empty();
    if (!answered) {
      Say(c, repo + ": " + error);
      return std::nullopt;
    }
    auto r = ParseReleases(json, c.asset, c.tag_prefix, beta);
    if (r) r->channel = beta ? "beta" : "stable";
    return r;
  };
  bool stable_answered = false, beta_answered = false;
  std::optional<Release> stable = latest(c.stable_repo, false, stable_answered), beta;
  if (task.cancelled()) return "Cancelled.";
  if (channel == "beta") beta = latest(c.beta_repo, true, beta_answered);
  if (task.cancelled()) return "Cancelled.";
  if (!stable_answered && !beta_answered) return kCheckFailed;
  UpdateCheck result;
  result.found = PickUpdate(channel, switching, c.current_version, stable, beta);
  Say(c, channel + ": stable " + (stable ? stable->version : "none") + ", beta " +
             (beta ? beta->version : channel == "beta" ? "none" : "not asked") + ", this is " + c.current_version);
  if (result.found)
    result.message = switching ? "Version " + result.found->version + " is the newest on the " + channel + " channel."
                               : "Version " + result.found->version + " is out. You have version " +
                                     c.current_version + ".";
  else if (channel == "beta" && switching)
    result.message = "There is no beta right now, so you stay on version " + c.current_version +
                     ". The next beta is offered as soon as it is out.";
  else
    result.message = "You have the newest version (" + c.current_version + ").";
  if (out) *out = result;
  return "";
}

std::string InstallUpdate(Task& task, const UpdaterConfig& c, const Release& r, InstallReport* out) {
  if (out) *out = InstallReport();
  if (EndsWith(r.asset_name, ".AppImage")) return InstallAppImage(task, c, r, out);
  const fs::path program = Utf8Path(c.program_dir), work = WorkDir(c);
  std::error_code ec;
  fs::path root;
  if (std::string error = FetchZip(task, c, r, work, root); !error.empty()) {
    fs::remove_all(work / "files", ec);
    return error;
  }
  if (task.cancelled()) return fs::remove_all(work / "files", ec), "Cancelled.";
  // From here on the install runs to its end (or its undo): Cancel is no longer offered.
  task.SetLabel(kInstallingLabel);
  task.Progress(0, -1);
  const up::ProtectedPaths keep = MakeKeep(c);
  const auto log = LogTo(c);
  const std::string src = ToUtf8(root), dst = c.program_dir;
  std::vector<std::string> plan;
  std::optional<up::Manifest> list, old;
  if (fs::exists(root / Utf8Path(c.files_list), ec)) {
    std::string error;
    list = up::ReadManifest(ToUtf8(root / Utf8Path(c.files_list)), &error);
    if (!list) {
      Say(c, c.files_list + ": " + error);
      fs::remove_all(work / "files", ec);
      return kListDamaged;
    }
    old = up::ReadManifest(ToUtf8(program / Utf8Path(c.files_list)));
    // Every listed file whose copy differs, each checked against the list before anything changes.
    plan = up::MissingOrWrong(dst, *list, keep, UINT64_MAX);
    for (const std::string& p : plan) {
      const up::FileEntry* f = list->Find(p);
      const fs::path s = root / Utf8Path(p);
      if (!fs::is_regular_file(s, ec) || fs::file_size(s, ec) != f->size || up::FileSha256(ToUtf8(s)) != f->sha256) {
        Say(c, "the update's copy of " + p + " does not match its file list");
        fs::remove_all(work / "files", ec);
        return kListDamaged;
      }
    }
    plan.push_back(c.files_list);  // last: until it is in place, the old list describes the folder
    Say(c, "version " + list->version + " lists " + std::to_string(list->files.size()) + " files; " +
               std::to_string(plan.size()) + " to install");
  } else {
    // A release without a file list (an older version, when changing channel): every file it has, nothing removed.
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const std::string rel = GenericUtf8(it->path().lexically_relative(root));
      if (up::SafePath(rel) && !keep(rel)) plan.push_back(rel);
    }
    Say(c, "version " + r.version + " has no " + c.files_list + ": " + std::to_string(plan.size()) + " files");
  }
  const up::InstallResult result = up::InstallFiles(src, dst, plan, log);
  if (result.outcome != up::Outcome::kOk) {
    fs::remove_all(work / "files", ec);  // the checked zip stays for a retry
    return InstallMessage(result);
  }
  int retired = 0;
  if (list) {
    // Files earlier releases shipped (the release's retired list, and the files the old list named that the new
    // one does not), removed only while unchanged.
    up::Manifest gone;
    if (const auto r2 = up::ReadManifest(ToUtf8(root / Utf8Path(c.retired_list)))) gone = *r2;
    if (old) gone.files.insert(gone.files.end(), old->files.begin(), old->files.end());
    up::ProtectedPaths retire_keep = keep;
    retire_keep.Add(c.files_list);
    retired = int(up::RetireFiles(dst, gone, *list, retire_keep, log).size());
  }
  fs::remove_all(work, ec);
  if (out) {
    out->installed = true;
    out->files = int(plan.size());
    out->retired = retired;
    out->message = "Version " + r.version + " is installed. Restart to use it.";
  }
  Say(c, "installed version " + r.version);
  return "";
}

std::string CheckProgramFiles(Task& task, const UpdaterConfig& c, InstallReport* out) {
  if (out) *out = InstallReport();
  task.SetLabel("Checking the game's own files");
  if (const char* a = std::getenv("APPIMAGE"); a && *a) return "";  // an AppImage is always complete
  const fs::path program = Utf8Path(c.program_dir);
  const std::string text = ReadText(program / Utf8Path(c.files_list));
  if (text.empty()) return "";  // not a packaged release
  const std::string passed = up::Sha256Hex(text.data(), text.size()) + "\n" + c.program_dir + "\n";
  if (!c.state_file.empty() && ReadText(Utf8Path(c.state_file)) == passed) return "";
  auto remember = [&] {
    if (c.state_file.empty()) return;
    std::error_code ec;
    fs::create_directories(Utf8Path(c.state_file).parent_path(), ec);
    std::ofstream(Utf8Path(c.state_file), std::ios::binary | std::ios::trunc) << passed;
  };
  std::string error;
  const auto list = up::ParseManifest(text, &error);
  if (!list) {
    Say(c, "files check: " + c.files_list + ": " + error);
    return kListDamaged;
  }
  if (CompareVersions(list->version, c.current_version) != 0) {
    Say(c, "files check: the list is for version " + list->version + ", this is " + c.current_version + ": skipped");
    return "";
  }
  const up::ProtectedPaths keep = MakeKeep(c);
  const auto log = LogTo(c);
  up::RemoveOldCopies(c.program_dir, *list);
  if (const auto retired = up::ReadManifest(ToUtf8(program / Utf8Path(c.retired_list)))) {
    up::ProtectedPaths retire_keep = keep;
    retire_keep.Add(c.files_list);
    if (out) out->retired = int(up::RetireFiles(c.program_dir, *retired, *list, retire_keep, log).size());
  }
  const auto needed = up::MissingOrWrong(c.program_dir, *list, keep, kCheapHashLimit);
  if (needed.empty()) {
    Say(c, "files check: all " + std::to_string(list->files.size()) + " files are in place");
    remember();
    return "";
  }
  Say(c, "files check: " + std::to_string(needed.size()) + " files missing or wrong, first " + needed[0]);
  {  // A folder it cannot write to would mean downloading the release at every start for nothing.
    const fs::path probe = program / "trg-write-test.tmp";
    std::error_code ec;
    const bool writable = static_cast<bool>(std::ofstream(probe, std::ios::binary | std::ios::trunc));
    fs::remove(probe, ec);
    if (!writable) return "The game folder is read only, so missing files cannot be fetched.";
  }
  // This version's own release: the beta repo for a beta version, when there is one.
  const std::string repo = IsPrerelease(c.current_version) && !c.beta_repo.empty() ? c.beta_repo : c.stable_repo;
  std::string json;
  if (std::string e = Get(c, c.api_base + "/repos/" + repo + "/releases/tags/" + c.tag_prefix + c.current_version,
                          json, &task);
      !e.empty())
    return e;
  const auto release = ParseReleases(json, c.asset, c.tag_prefix, true);
  if (!release || CompareVersions(release->version, c.current_version) != 0)
    return "Could not find the release of version " + c.current_version + ".";
  const fs::path work = WorkDir(c);
  std::error_code ec;
  fs::path root;
  if (std::string e = FetchZip(task, c, *release, work, root); !e.empty()) {
    fs::remove_all(work / "files", ec);
    return e;
  }
  if (task.cancelled()) return fs::remove_all(work / "files", ec), "Cancelled.";
  task.SetLabel(kInstallingLabel);
  const auto r = up::InstallFromSource(ToUtf8(root), c.program_dir, *list, needed, log);
  if (r.install.outcome != up::Outcome::kOk) {
    fs::remove_all(work / "files", ec);
    return InstallMessage(r.install);
  }
  fs::remove_all(work, ec);
  // Files the release itself lacks would only be fetched again at every start: remembered as checked.
  remember();
  if (out) {
    out->installed = !r.installed.empty();
    out->files = int(r.installed.size());
    out->message = "Every file of version " + c.current_version + " is in place again.";
  }
  Say(c, "files check: " + std::to_string(r.installed.size()) + " files installed, " +
             std::to_string(r.unavailable.size()) + " not in the release");
  return "";
}

void CleanUpAfterUpdate(const UpdaterConfig& c) {
  const fs::path program = Utf8Path(c.program_dir);
  for (const std::string* name : {&c.files_list, &c.retired_list})
    if (const auto list = up::ReadManifest(ToUtf8(program / Utf8Path(*name))))
      up::RemoveOldCopies(c.program_dir, *list);
  std::error_code ec;
  for (const std::string* name : {&c.files_list, &c.program_name})
    if (!name->empty()) fs::remove(program / Utf8Path(*name + ".old"), ec);
}

bool RestartProgram() {
#if defined(_WIN32)
  std::wstring cmd = GetCommandLineW();
  wchar_t exe[32768] = L"";
  if (!GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)))) return false;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#elif defined(__linux__)
  std::vector<std::string> args;
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  for (std::string arg; std::getline(cmdline, arg, '\0');) args.push_back(arg);
  const char* appimage = std::getenv("APPIMAGE");  // the AppImage itself, not its mounted program
  std::error_code ec;
  const std::string exe = appimage && *appimage ? appimage : fs::read_symlink("/proc/self/exe", ec).string();
  if (exe.empty()) return false;
  std::vector<char*> argv{const_cast<char*>(exe.c_str())};
  for (size_t i = 1; i < args.size(); ++i) argv.push_back(args[i].data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  return posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) == 0;
#else
  return false;  // TODO(macOS)
#endif
}

// ------------------------------------------------------------------- UI ---
void StartUpdateCheck(UpdatesState& st, const UpdaterConfig& config, const std::string& channel, bool switching) {
  if (st.task.running() || st.installing || st.restart_ready) return;
  st.checking = true;
  st.check = UpdateCheck();
  st.message.clear();
  UpdateCheck* out = &st.check;
  st.task.Start("Checking for updates", [config, channel, switching, out](Task& t) {
    return CheckForUpdate(t, config, channel, switching, out);
  });
}

void StartProgramFilesCheck(UpdatesState& st, const UpdaterConfig& config) {
  if (st.files.running()) return;
  InstallReport* out = &st.files_report;
  st.files.Start("Checking the game's own files", [config, out](Task& t) { return CheckProgramFiles(t, config, out); });
}

bool UpdateWaiting(const UpdatesState& st) {
  // Also before the rows collect the check's result, so the sidebar dot shows on any page.
  return !st.task.running() && !st.installing && !st.restart_ready && st.check.found.has_value();
}

void UpdatesRows(Ui& ui, UpdatesState& st, const UpdaterConfig& config, const UpdatesRowOptions& opt) {
  std::string error;
  if (const Task::State s = st.task.Finish(&error); s != Task::State::kIdle) {
    if (st.checking) {
      st.checking = false;
      st.message = s == Task::State::kDone ? st.check.message : error.empty() ? "Cancelled." : error;
    } else if (st.installing) {
      st.installing = false;
      st.restart_ready = st.report.installed;
      st.message = st.report.installed ? st.report.message : error.empty() ? "Cancelled." : error;
      if (st.report.installed) st.check.found.reset();
    }
  }
  if (const Task::State s = st.files.Finish(&error); s != Task::State::kIdle) {
    if (!error.empty() && config.log) config.log("update: files check: " + error);
    if (st.files_report.installed && st.message.empty()) st.message = st.files_report.message;
  }

  const bool busy = st.task.running(), files_busy = st.files.running();
  std::string version = config.current_version;
  if (IsPrerelease(version)) version += "  (beta)";
  ui.Info("Version", version.c_str());
  std::string channel = opt.channel_key ? ui.settings().Get(opt.channel_key, opt.default_channel) : "stable";
  if (!config.beta_repo.empty() && opt.channel_key) {
    ImGui::BeginDisabled(busy || st.restart_ready);
    if (ui.Choice("Update channel",
                  "Beta versions are test builds of what is coming next: they can have bugs. Changing channel looks "
                  "for that channel's newest version; nothing is installed until you press Install.",
                  opt.channel_key, opt.default_channel, {{"stable", "Stable"}, {"beta", "Beta"}})) {
      channel = ui.settings().Get(opt.channel_key, opt.default_channel);
      StartUpdateCheck(st, config, channel, true);
    }
    ImGui::EndDisabled();
  }
  if (opt.check_key)
    ui.Toggle("Check at startup",
              "Looks for a new version each time the launcher opens. Nothing is installed without asking.",
              opt.check_key, true);

  ui.Row("Updates",
         "Downloads the new version from GitHub, checks it and replaces the port's own files. Your installed game, "
         "saves and settings stay as they are.");
  if (busy || files_busy) {
    Task& task = busy ? st.task : st.files;  // the first start check has no Cancel: it runs once, quietly
    const float f = task.fraction();
    const std::string label = task.label();
    const bool can_cancel = busy && label != kInstallingLabel;
    char text[200];
    if (f >= 0.0f)
      std::snprintf(text, sizeof text, "%s %.0f%%", label.c_str(), double(f) * 100.0);
    else
      std::snprintf(text, sizeof text, "%s", label.c_str());
    const float cancel_w = can_cancel ? 120 * ui.scale() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
    ImGui::ProgressBar(f >= 0.0f ? f : -1.0f * float(ImGui::GetTime()),
                       ImVec2(ImGui::GetContentRegionAvail().x - cancel_w, 0), text);
    if (can_cancel) {
      ImGui::SameLine();
      ImGui::BeginDisabled(task.cancelled());
      if (ImGui::Button("Cancel##update", ImVec2(-FLT_MIN, 0))) st.task.Cancel();
      ImGui::EndDisabled();
    }
  } else if (st.restart_ready) {
    if (ui.AccentButton("Restart now")) {
      if (opt.on_restart) {
        opt.on_restart();
      } else {
        ui.launcher().Save();
        if (RestartProgram())
          ui.launcher().RequestQuit();
        else
          st.message = "Could not restart by itself. Close the launcher and start it again.";
      }
    }
  } else if (UpdateWaiting(st)) {
    const Release r = *st.check.found;
    if (ui.AccentButton(("Install version " + r.version).c_str())) {
      st.installing = true;
      st.message.clear();
      InstallReport* out = &st.report;
      st.task.Start("Downloading version " + r.version,
                    [config, r, out](Task& t) { return InstallUpdate(t, config, r, out); });
    }
    if (!r.page_url.empty() && ImGui::Button("What's new", ImVec2(-FLT_MIN, 0))) OpenUrl(r.page_url);
  } else if (ImGui::Button("Check for updates", ImVec2(-FLT_MIN, 0))) {
    StartUpdateCheck(st, config, channel, false);
  }
  if (!st.message.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().dim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(st.message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  }
}

}  // namespace trg
