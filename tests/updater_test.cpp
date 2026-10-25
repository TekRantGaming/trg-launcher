// The updater (updater.h) on real folders: version order, release picking,
// file lists, the staged install with undo, retired files, protected paths and
// the first start check. GitHub is replaced by a table of local answers and
// the release zips are written here, so nothing touches the network.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "trg/updater.h"

namespace {

namespace fs = std::filesystem;
namespace up = trg::update;

int failures = 0;

void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

void Write(const fs::path& p, const std::string& data) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary | std::ios::trunc).write(data.data(), std::streamsize(data.size()));
}

std::string Read(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string U8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

// ------------------------------------------------------------ test zips ---
uint32_t Crc32(const std::string& d) {
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : d) {
    c ^= b;
    for (int k = 0; k < 8; ++k) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
  }
  return ~c;
}

void Put16(std::string& s, uint32_t v) { s += char(v & 0xFF), s += char((v >> 8) & 0xFF); }
void Put32(std::string& s, uint32_t v) { Put16(s, v & 0xFFFF), Put16(s, v >> 16); }

// A zip with stored (uncompressed) entries, as any unzip tool reads it.
std::string MakeZip(const std::map<std::string, std::string>& files) {
  std::string zip, dir;
  for (const auto& [name, data] : files) {
    const uint32_t offset = uint32_t(zip.size()), crc = Crc32(data), size = uint32_t(data.size());
    auto common = [&](std::string& s) {
      Put16(s, 20), Put16(s, 0x0800), Put16(s, 0), Put16(s, 0), Put16(s, 0x21);  // version, UTF-8, stored, 1980
      Put32(s, crc), Put32(s, size), Put32(s, size), Put16(s, uint32_t(name.size())), Put16(s, 0);
    };
    Put32(zip, 0x04034b50);
    common(zip);
    zip += name + data;
    Put32(dir, 0x02014b50);
    Put16(dir, 20);
    common(dir);
    Put16(dir, 0), Put16(dir, 0), Put16(dir, 0), Put32(dir, 0), Put32(dir, offset);
    dir += name;
  }
  const uint32_t dir_at = uint32_t(zip.size());
  zip += dir;
  Put32(zip, 0x06054b50), Put16(zip, 0), Put16(zip, 0), Put16(zip, uint32_t(files.size()));
  Put16(zip, uint32_t(files.size())), Put32(zip, uint32_t(dir.size())), Put32(zip, dir_at), Put16(zip, 0);
  return zip;
}

// A file list line set for `files` (path -> contents), as tools/make_update_files.py writes it.
std::string Lines(const std::map<std::string, std::string>& files) {
  std::string out;
  for (const auto& [path, data] : files)
    out += up::Sha256Hex(data.data(), data.size()) + " " + std::to_string(data.size()) + " " + path + "\n";
  return out;
}
std::string ListOf(const std::string& version, const std::map<std::string, std::string>& files) {
  return "# test list\nversion " + version + "\n" + Lines(files);
}

// GitHub, as a table of answers. Unknown addresses fail like a dead connection.
struct FakeGitHub {
  std::map<std::string, std::string> answers;
  int calls = 0;
  trg::HttpFn Fn() {
    return [this](const std::string& url, std::string& out, trg::Task*) -> std::string {
      ++calls;
      const auto it = answers.find(url);
      if (it == answers.end()) return "Could not reach the server.";
      out = it->second;
      return "";
    };
  }
};

std::string ReleaseJson(const std::string& tag, const std::string& asset, uint64_t size, bool sums,
                        bool draft = false, bool prerelease = false) {
  std::string j = "{\"tag_name\":\"" + tag + "\",\"html_url\":\"https://example.test/" + tag + "\",\"draft\":" +
                  (draft ? "true" : "false") + ",\"prerelease\":" + (prerelease ? "true" : "false") +
                  ",\"assets\":[{\"name\":\"" + asset + "\",\"size\":" + std::to_string(size) +
                  ",\"browser_download_url\":\"https://dl.test/" + tag + "/" + asset + "\"}";
  if (sums)
    j += ",{\"name\":\"SHA256SUMS.txt\",\"size\":100,\"browser_download_url\":\"https://dl.test/" + tag +
         "/SHA256SUMS.txt\"}";
  return j + "]}";
}

trg::Release R(const char* v) {
  trg::Release r;
  r.version = v;
  return r;
}

// ---------------------------------------------------------------- tests ---
void Versions() {
  using trg::CompareVersions;
  Check(CompareVersions("1.4.1", "v1.4.1") == 0 && CompareVersions("1.4", "1.4.0") == 0, "v prefix and 1.4 == 1.4.0");
  Check(CompareVersions("1.10.0", "1.9.12") > 0, "1.10.0 > 1.9.12");
  Check(CompareVersions("2.0.0-beta.2", "2.0.0") < 0, "a beta comes before its release");
  Check(CompareVersions("2.0.0-beta.10", "2.0.0-beta.9") > 0, "beta.10 > beta.9");
  Check(CompareVersions("2.0.0-beta10", "2.0.0-beta.9") > 0, "beta10 reads as beta.10");
  Check(CompareVersions("2.0.0-beta.1", "1.9.13") > 0, "2.0.0-beta.1 > 1.9.13");
  Check(CompareVersions("2.0.0-rc.1", "2.0.0-beta.5") > 0, "rc after beta");
  Check(CompareVersions("1.2.3+build7", "1.2.3") == 0, "build metadata is ignored");
  Check(trg::IsPrerelease("2.0.0-beta.3") && !trg::IsPrerelease("v2.0.0"), "IsPrerelease");
}

void Picking() {
  const std::optional<trg::Release> none;
  struct Case {
    const char* name;
    const char* channel;
    bool switching;
    const char* current;
    std::optional<trg::Release> stable, beta;
    const char* expect;  // "" for nothing
  };
  const Case cases[] = {
      {"beta sees the next beta", "beta", false, "2.0.0-beta.1", R("1.9.11"), R("2.0.0-beta.2"), "2.0.0-beta.2"},
      {"beta ignores an older line's hotfix", "beta", false, "2.0.0-beta.1", R("1.9.12"), R("2.0.0-beta.1"), ""},
      {"beta ignores the hotfix with no beta", "beta", false, "2.0.0-beta.1", R("1.9.12"), none, ""},
      {"beta takes its final release", "beta", false, "2.0.0-beta.3", R("2.0.0"), R("2.0.0-beta.3"), "2.0.0"},
      {"final release beats an older beta", "beta", false, "2.0.0-beta.1", R("2.0.0"), R("2.0.0-beta.2"), "2.0.0"},
      {"beta takes a later stable", "beta", false, "2.0.0-beta.3", R("2.1.0"), none, "2.1.0"},
      {"no answers: nothing", "beta", false, "2.0.0-beta.1", none, none, ""},
      {"stable ignores betas", "stable", false, "1.9.11", R("1.9.11"), R("2.0.0-beta.2"), ""},
      {"stable sees a newer stable", "stable", false, "1.9.11", R("1.9.12"), R("2.0.0-beta.2"), "1.9.12"},
      {"stable never goes down", "stable", false, "1.9.12", R("1.9.11"), none, ""},
      {"switch to stable may go down", "stable", true, "2.0.0-beta.1", R("1.9.11"), R("2.0.0-beta.1"), "1.9.11"},
      {"switch to beta", "beta", true, "1.9.11", R("1.9.11"), R("2.0.0-beta.1"), "2.0.0-beta.1"},
      {"switch to beta, no beta", "beta", true, "1.9.11", R("1.9.11"), none, ""},
      {"switch to the version already running", "stable", true, "1.9.11", R("1.9.11"), none, ""},
  };
  for (const Case& c : cases) {
    const auto got = trg::PickUpdate(c.channel, c.switching, c.current, c.stable, c.beta);
    Check((got ? got->version : "") == std::string(c.expect), std::string("pick: ") + c.name);
  }
}

void Releases() {
  Check(trg::MatchesPattern("Game-v1.2.0-windows-x64.zip", "*windows-x64.zip"), "pattern matches the zip");
  Check(!trg::MatchesPattern("Game-v1.2.0-windows-x64.zip.json", "*windows-x64.zip"), "pattern needs the whole name");
  Check(trg::MatchesPattern("Game-v1.2.0-linux-x86_64.AppImage", "Game-v*-linux-*.AppImage"), "two wildcards");
  const std::string list = "[" + ReleaseJson("v1.3.0", "G-v1.3.0-windows-x64.zip", 10, true, true) + "," +
                           ReleaseJson("v1.2.5-beta.1", "G-v1.2.5-beta.1-windows-x64.zip", 11, true, false, true) +
                           "," + ReleaseJson("v1.2.1", "G-v1.2.1-windows-x64.zip", 12, false) + "," +
                           ReleaseJson("v1.2.4", "G-v1.2.4-linux-x86_64.AppImage", 13, true) + "," +
                           ReleaseJson("v1.2.0", "G-v1.2.0-windows-x64.zip", 14, true) + "]";
  const auto stable = trg::ParseReleases(list, "*windows-x64.zip", "v", false);
  Check(stable && stable->version == "1.2.1" && stable->asset_size == 12 && stable->sums_url.empty(),
        "stable: the newest with a zip, drafts and prereleases skipped");
  const auto beta = trg::ParseReleases(list, "*windows-x64.zip", "v", true);
  Check(beta && beta->version == "1.2.5-beta.1" && !beta->sums_url.empty(), "beta: prereleases count");
  const auto one = trg::ParseReleases(ReleaseJson("v1.2.0", "G-v1.2.0-windows-x64.zip", 14, true), "*windows-x64.zip",
                                      "v", true);
  Check(one && one->asset_url == "https://dl.test/v1.2.0/G-v1.2.0-windows-x64.zip", "one release object");
  Check(!trg::ParseReleases("{\"message\":\"Not Found\"}", "*.zip", "v", true), "an error answer has no release");
  Check(!trg::ParseReleases("[{\"tag_name\":", "*.zip", "v", true), "broken JSON is refused");
  const auto escaped = trg::ParseReleases(
      "[{\"tag_name\":\"v1.0.0\",\"html_url\":\"a\\/b \\u00e9\",\"assets\":[{\"name\":\"x.zip\",\"size\":1,"
      "\"browser_download_url\":\"u\"}]}]",
      "*.zip", "v", false);
  Check(escaped && escaped->page_url == "a/b \xC3\xA9", "JSON escapes");
}

void Lists() {
  const std::string h = std::string(64, 'a');
  const auto m = up::ParseManifest("\xEF\xBB\xBF# comment\r\nversion 1.2.0\r\n" + h + " 5 my folder/file one.txt\r\n" +
                                   std::string(64, 'B') + " 0 empty\n");
  Check(m && m->version == "1.2.0" && m->files.size() == 2 && m->files[0].path == "my folder/file one.txt" &&
            m->files[1].sha256 == std::string(64, 'b'),
        "list: BOM, CRLF, spaces in paths, upper case hashes");
  Check(m && m->Find("MY FOLDER/File One.txt") != nullptr, "list: Find ignores letter case");
  std::string error;
  Check(!up::ParseManifest(h.substr(1) + " 5 a\n", &error) && error.find("line 1") == 0, "list: short hash refused");
  Check(!up::ParseManifest(h + " 5x a\n"), "list: bad size refused");
  Check(!up::ParseManifest(h + " 5\n"), "list: line without a path refused");
  for (const char* bad : {"../x", "/x", "a\\b", "C:x", "a//b", "a/./b", "a/", "a/.."})
    Check(!up::SafePath(bad) && !up::ParseManifest(h + " 1 " + bad + "\n"), std::string("unsafe path refused: ") + bad);
  Check(up::SafePath("launcher/video/intro.mp4") && up::SafePath("x..y"), "safe paths pass");

  Check(up::Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
  Check(up::Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
  const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  Check(up::Sha256Hex(two.data(), two.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "sha256 two blocks");
  const std::string sums = h + "  Game-v1.2.0-windows-x64.zip\r\n" + std::string(64, 'c') + " *Game.AppImage\n";
  Check(up::SumFor(sums, "Game-v1.2.0-windows-x64.zip") == h &&
            up::SumFor(sums, "Game.AppImage") == std::string(64, 'c') &&
            up::SumFor(sums, "other.zip").empty(),
        "SHA256SUMS.txt lines");
}

void Protection(const fs::path& work) {
  const fs::path program = work / "protect";
  fs::create_directories(program / "user");
  const up::ProtectedPaths keep({"game", "Settings.toml", U8(program / "user"), U8(work / "elsewhere"), "saves/"},
                                U8(program));
  Check(keep("game/disc.iso") && keep("GAME") && keep("settings.toml") && keep("user/mods/a.txt") &&
            keep("saves/1.sav"),
        "protected: folders, files, absolute folders, letter case");
  Check(!keep("gamex/a") && !keep("game.exe") && !keep("elsewhere/a") && !keep("settings.toml.bak"),
        "protected: nothing else");
}

// An old install: the program, a data folder, a player's own file and protected data.
const std::map<std::string, std::string> kOld = {
    {"game.exe", "old program"},          {"engine.dll", "old engine"},  {"data/levels.pak", "old levels"},
    {"data/gone.txt", "shipped by 1.0"},  {"docs/old-notes.txt", "notes"}, {"changed.txt", "shipped, then edited"},
};

void SetUpOld(const fs::path& program) {
  fs::remove_all(program);
  for (const auto& [p, d] : kOld) Write(program / p, d);
  std::map<std::string, std::string> listed = kOld;
  listed["changed.txt"] = "shipped";  // the player edited it since
  Write(program / "update-files.txt", ListOf("1.0.0", listed));
  Write(program / "game/disc.iso", "the player's game");
  Write(program / "settings.toml", "player settings");
  Write(program / "reshade.ini", "the player's own file");
}

// The new release: data/gone.txt, docs/ and changed.txt retired, a new folder.
std::map<std::string, std::string> NewFiles() {
  std::map<std::string, std::string> f = {
      {"game.exe", "new program"},     {"engine.dll", "old engine"},    {"data/levels.pak", "new levels"},
      {"tools/new/helper.dll", "new"}, {"readme.txt", "read me"},       {"game/disc.iso", "NOT FOR PLAYERS"},
  };
  f["update-retired.txt"] = "version 1.1.0\n" + Lines({{"data/gone.txt", "shipped by 1.0"}});
  return f;
}

void FileWork(const fs::path& work) {
  const fs::path program = work / "files", src = work / "files_src";
  SetUpOld(program);
  fs::remove_all(src);
  for (const auto& [p, d] : NewFiles()) Write(src / p, d);
  const up::ProtectedPaths keep({"game", "settings.toml"}, U8(program));
  const auto list = up::ParseManifest(ListOf("1.1.0", NewFiles()));
  auto plan = up::MissingOrWrong(U8(program), *list, keep, UINT64_MAX);
  Check(plan.size() == 5, "plan: changed and new files only, never game/ (" + std::to_string(plan.size()) + ")");

  // A swap that fails halfway: a folder sits where readme.txt's .old copy would go... and readme.txt exists.
  Write(program / "readme.txt", "old readme");
  Write(program / "readme.txt.old/blocker", "x");
  plan = up::MissingOrWrong(U8(program), *list, keep, UINT64_MAX);
  const auto failed = up::InstallFiles(U8(src), U8(program), plan);
  Check(failed.outcome == up::Outcome::kInUse && failed.path == "readme.txt", "a failed swap is reported");
  Check(Read(program / "game.exe") == "old program" && Read(program / "data/levels.pak") == "old levels" &&
            Read(program / "readme.txt") == "old readme" && !fs::exists(program / "tools") &&
            !fs::exists(program / "game.exe.new") && !fs::exists(program / "game.exe.old"),
        "undo: every old file back, new folders and staged files gone");
  fs::remove_all(program / "readme.txt.old");
  fs::remove(program / "readme.txt");

#if defined(_WIN32)
  {  // A file held open by another program cannot be renamed on Windows.
    FILE* held = _wfopen((program / "engine.dll").wstring().c_str(), L"rb");
    const auto r = up::InstallFiles(U8(src), U8(program), {"game.exe", "engine.dll"});
    std::fclose(held);
    Check(r.outcome == up::Outcome::kInUse && r.path == "engine.dll" && Read(program / "game.exe") == "old program",
          "a file in use undoes the update");
  }
#endif

  plan = up::MissingOrWrong(U8(program), *list, keep, UINT64_MAX);
  const auto ok = up::InstallFiles(U8(src), U8(program), plan);
  Check(ok.outcome == up::Outcome::kOk && Read(program / "game.exe") == "new program" &&
            Read(program / "tools/new/helper.dll") == "new" && Read(program / "game/disc.iso") == "the player's game" &&
            !fs::exists(program / "game.exe.old"),
        "install: new files in, game/ untouched, no .old left");
  Check(up::MissingOrWrong(U8(program), *list, keep, UINT64_MAX).empty(), "install: the folder matches the list");

  // Retired: the release's list plus what the old list named that the new one does not.
  up::Manifest gone = *up::ParseManifest(Read(src / "update-retired.txt"));
  std::map<std::string, std::string> old_listed = kOld;
  old_listed["changed.txt"] = "shipped";
  old_listed["game/disc.iso"] = "the player's game";  // even if a list names it, it is protected
  const auto old = up::ParseManifest(ListOf("1.0.0", old_listed));
  gone.files.insert(gone.files.end(), old->files.begin(), old->files.end());
  const auto removed = up::RetireFiles(U8(program), gone, *list, keep);
  Check(removed.size() == 2 && !fs::exists(program / "data/gone.txt") && !fs::exists(program / "docs"),
        "retire: unchanged files removed and their empty folder");
  Check(Read(program / "changed.txt") == "shipped, then edited" &&
            Read(program / "reshade.ini") == "the player's own file" &&
            Read(program / "engine.dll") == "old engine" && Read(program / "game/disc.iso") == "the player's game" &&
            fs::exists(program / "data/levels.pak"),
        "retire: edited, unknown, still listed and protected files kept");
}

trg::UpdaterConfig Config(const fs::path& work, const fs::path& program, FakeGitHub& gh, const char* version) {
  trg::UpdaterConfig c;
  c.stable_repo = "Owner/game";
  c.beta_repo = "Owner/game-beta";
  c.current_version = version;
  c.asset = "*windows-x64.zip";
  c.program_dir = U8(program);
  c.program_name = "game.exe";
  c.protected_paths = {"game", "settings.toml"};
  c.work_dir = U8(work / "update-work");
  c.http = gh.Fn();
  c.log = [](const std::string& line) { std::printf("        %s\n", line.c_str()); };
  return c;
}

// Publishes a release zip ("Game/" folder inside) with its list on the fake GitHub.
void Publish(FakeGitHub& gh, const std::string& tag, std::map<std::string, std::string> files, bool sums = true,
             bool list = true, const std::string& repo = "Owner/game") {
  const std::string version = tag.substr(1);
  if (list) files["update-files.txt"] = ListOf(version, files);
  std::map<std::string, std::string> entries;
  for (const auto& [p, d] : files) entries["Game/" + p] = d;
  const std::string asset = "Game-" + tag + "-windows-x64.zip", zip = MakeZip(entries);
  const std::string json = ReleaseJson(tag, asset, zip.size(), sums);
  gh.answers["https://api.github.com/repos/" + repo + "/releases?per_page=20"] = "[" + json + "]";
  gh.answers["https://api.github.com/repos/" + repo + "/releases/tags/" + tag] = json;
  gh.answers["https://dl.test/" + tag + "/" + asset] = zip;
  gh.answers["https://dl.test/" + tag + "/SHA256SUMS.txt"] =
      up::Sha256Hex(zip.data(), zip.size()) + "  " + asset + "\n";
}

void FullUpdate(const fs::path& work) {
  const fs::path program = work / "full";
  SetUpOld(program);
  FakeGitHub gh;
  Publish(gh, "v1.1.0", NewFiles());
  auto c = Config(work, program, gh, "1.0.0");
  trg::Task task;
  trg::UpdateCheck check;
  Check(trg::CheckForUpdate(task, c, "stable", false, &check).empty() && check.found &&
            check.found->version == "1.1.0" && check.found->sums_url.size(),
        "check: 1.1.0 found");
  Check(trg::CheckForUpdate(task, c, "beta", false, &check).empty() && check.found && check.found->version == "1.1.0",
        "check: a beta repo with no answer still sees stable");

  // A download that does not match SHA256SUMS.txt changes nothing.
  const std::string zip_url = check.found->asset_url, good = gh.answers[zip_url];
  gh.answers[zip_url][100] ^= 1;
  trg::InstallReport report;
  std::string error = trg::InstallUpdate(task, c, *check.found, &report);
  Check(!error.empty() && !report.installed && Read(program / "game.exe") == "old program",
        "install: a damaged download is refused (" + error + ")");
  gh.answers[zip_url] = good;

  error = trg::InstallUpdate(task, c, *check.found, &report);
  Check(error.empty() && report.installed, "install: done (" + error + ")");
  Check(Read(program / "game.exe") == "new program" && Read(program / "tools/new/helper.dll") == "new" &&
            up::ReadManifest(U8(program / "update-files.txt"))->version == "1.1.0",
        "install: every listed file, the new list last");
  Check(Read(program / "game/disc.iso") == "the player's game" &&
            Read(program / "settings.toml") == "player settings" &&
            Read(program / "reshade.ini") == "the player's own file" &&
            Read(program / "changed.txt") == "shipped, then edited",
        "install: protected, unknown and edited files untouched");
  Check(!fs::exists(program / "data/gone.txt") && !fs::exists(program / "docs") && report.retired == 2,
        "install: retired files removed (" + std::to_string(report.retired) + ")");
  Check(!fs::exists(work / "update-work"), "install: the work folder is cleaned up");

  // Now the same version: nothing to offer. Then a release without SHA256SUMS.txt and a wrong size.
  c.current_version = "1.1.0";
  Check(trg::CheckForUpdate(task, c, "stable", false, &check).empty() && !check.found, "check: up to date");
  Publish(gh, "v1.2.0", NewFiles(), false);
  Check(trg::CheckForUpdate(task, c, "stable", false, &check).empty() && check.found && check.found->sums_url.empty(),
        "check: 1.2.0 without SHA256SUMS.txt");
  trg::Release wrong = *check.found;
  wrong.asset_size += 1;
  Check(!trg::InstallUpdate(task, c, wrong, &report).empty() && !report.installed,
        "install: without SHA256SUMS.txt a wrong size is refused");

  // A release without a file list (switching back to an old version): its files, protected paths aside.
  const fs::path plain = work / "plain";
  SetUpOld(plain);
  Publish(gh, "v0.9.0", {{"game.exe", "very old program"}, {"game/disc.iso", "NO"}, {"extra/a.txt", "a"}}, true, false);
  auto c2 = Config(work, plain, gh, "1.0.0");
  Check(trg::CheckForUpdate(task, c2, "stable", true, &check).empty() && check.found && check.found->version == "0.9.0",
        "switch: an older version is offered");
  Check(trg::InstallUpdate(task, c2, *check.found, &report).empty() && Read(plain / "game.exe") == "very old program" &&
            Read(plain / "extra/a.txt") == "a" && Read(plain / "game/disc.iso") == "the player's game" &&
            Read(plain / "data/gone.txt") == "shipped by 1.0",
        "install without a list: every file but protected ones, nothing removed");

  Check(trg::CheckForUpdate(task, c2, "stable", false, &check).empty() && !check.found,
        "check: an older stable is not an update");
  gh.answers.clear();
  Check(!trg::CheckForUpdate(task, c2, "stable", false, &check).empty(), "check: no answer at all says so");
}

void FirstStart(const fs::path& work) {
  const fs::path program = work / "first";
  fs::remove_all(program);
  FakeGitHub gh;
  auto files = NewFiles();
  files.erase("game/disc.iso");
  Publish(gh, "v1.1.0", files);
  // What an older updater left: the program and list, but not tools/ and an old readme.
  for (const auto& [p, d] : files)
    if (p.rfind("tools/", 0) != 0) Write(program / p, p == "readme.txt" ? "old readme" : d);
  Write(program / "update-files.txt", ListOf("1.1.0", files));
  Write(program / "data/gone.txt", "shipped by 1.0");
  Write(program / "game/disc.iso", "the player's game");
  Write(program / "game.exe.old", "left by a swap");
  auto c = Config(work, program, gh, "1.1.0");
  c.state_file = U8(work / "first-state.txt");
  fs::remove(c.state_file);
  trg::CleanUpAfterUpdate(c);
  Check(!fs::exists(program / "game.exe.old"), "clean up: .old copies removed");

  trg::Task task;
  trg::InstallReport report;
  const std::string error = trg::CheckProgramFiles(task, c, &report);
  Check(error.empty() && report.installed && report.files == 2, "first start: 2 files fetched (" + error + ")");
  Check(Read(program / "tools/new/helper.dll") == "new" && Read(program / "readme.txt") == "read me" &&
            !fs::exists(program / "data/gone.txt") && Read(program / "game/disc.iso") == "the player's game",
        "first start: missing and wrong files fixed, retired removed, game/ untouched");
  const int calls = gh.calls;
  Check(trg::CheckProgramFiles(task, c, &report).empty() && gh.calls == calls && !report.installed,
        "first start: a passed check is remembered");

  // Another version's list, or no list at all: nothing happens.
  fs::remove(c.state_file);
  c.current_version = "1.2.0";
  Check(trg::CheckProgramFiles(task, c, &report).empty() && gh.calls == calls,
        "first start: another version's list is skipped");
  c.files_list = "none.txt";
  Check(trg::CheckProgramFiles(task, c, &report).empty() && gh.calls == calls, "first start: no list, nothing to do");
}

}  // namespace

int main() {
  const fs::path work = fs::absolute("updater_test_work");
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work);
  Versions();
  Picking();
  Releases();
  Lists();
  Protection(work);
  FileWork(work);
  FullUpdate(work);
  FirstStart(work);
  if (!failures) fs::remove_all(work, ec);
  std::puts(failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
