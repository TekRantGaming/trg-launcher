#include "games.h"

#include <cstdlib>
#include <filesystem>

#include <trg/updater.h>

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

fs::path PathOf(const std::string& utf8) { return fs::u8path(utf8); }

}  // namespace

const std::vector<GameInfo>& Games() { return Project().games; }
int GameCount() { return int(Project().games.size()); }

std::string ProgramDir() {
  static const std::string dir = [] {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    if (n > 0 && n < std::size(buf)) return Utf8(fs::path(buf).parent_path());
#else
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return Utf8(exe.parent_path());
#endif
    return Utf8(fs::current_path());
  }();
  return dir;
}

#if defined(_WIN32)
std::string LauncherDir() { return ProgramDir(); }
#else
namespace {
// XDG_DATA_HOME, or ~/.local/share: where the SDK keeps each game's user folder too.
std::string DataHome() {
  if (const char* x = getenv("XDG_DATA_HOME"); x && *x) return x;
  return std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.local/share";
}
}  // namespace

// On Linux the launcher and the games are inside the read only AppImage: everything written goes to the user's
// data folder.
std::string LauncherDir() {
  static const std::string dir = DataHome() + "/" + Project().linux_data_folder;
  return dir;
}
#endif

std::string GameDir(const GameInfo& game) { return LauncherDir() + "/games/" + game.id; }
std::string GameDataDir(const GameInfo& game) { return GameDir(game) + "/game"; }
#if defined(_WIN32)
std::string GameExePath(const GameInfo& game) { return GameDir(game) + "/" + game.exe; }
std::string GameSettingsPath(const GameInfo& game) { return GameDir(game) + "/" + game.id + ".toml"; }
#else
// The games come inside the AppImage, beside the launcher, without ".exe"; each reads its settings from its user
// folder (the games' OnConfigurePaths).
std::string GameExePath(const GameInfo& game) {
  std::string exe = game.exe;
  if (exe.size() > 4 && exe.compare(exe.size() - 4, 4, ".exe") == 0) exe.resize(exe.size() - 4);
  return ProgramDir() + "/" + exe;
}
std::string GameSettingsPath(const GameInfo& game) { return SavesDir(game) + "/" + game.id + ".toml"; }
#endif
std::string BackupsDir(const GameInfo& game) { return LauncherDir() + "/save-backups/" + game.id; }
std::string CacheDir() { return LauncherDir() + "/cache"; }

std::string SavesDir(const GameInfo& game) {
#if defined(_WIN32)
  PWSTR docs = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs))) {
    out = Utf8(fs::path(docs) / game.id);
  }
  CoTaskMemFree(docs);
  return out;
#else
  return DataHome() + "/" + game.id;  // the SDK's user folder for the game
#endif
}

GameState DetectState(const GameInfo& game) {
  std::error_code ec;
  if (!fs::exists(PathOf(GameExePath(game)), ec)) return GameState::kNotDownloaded;
  if (!fs::exists(PathOf(GameDataDir(game) + "/default.xex"), ec)) return GameState::kDownloaded;
  return GameState::kInstalled;
}

std::string InstalledVersion(const GameInfo& game) {
#if defined(_WIN32)
  auto manifest = trg::update::ReadManifest(GameDir(game) + "/update-files.txt");
  return manifest ? manifest->version : std::string();
#else
  (void)game;
  return Project().version;  // the games come inside the launcher's AppImage and update with it
#endif
}

}  // namespace trg::hub
