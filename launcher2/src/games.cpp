#include "games.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include <trg/settings.h>
#include <trg/updater.h>

#include "psx.h"

#include <SDL.h>

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

// Features::output_resolution: how a game shows a picture `height` pixels high (16:9). Its own video mode when it
// draws at that height by itself (GameInfo::native_heights), otherwise a native height times the draw scale: an exact
// match if there is one (the larger native height first), else the smallest size above, so the picture is drawn at
// least as large as it is shown.
namespace {

struct OutputSize {
  int mode_height = 720;
  int scale = 1;
};
OutputSize OutputFor(const GameInfo& game, int height) {
  std::vector<int> natives;
  for (const char* c = game.native_heights ? game.native_heights : "720"; *c;) {
    natives.push_back(std::atoi(c));
    while (*c && *c != ',') ++c;
    if (*c) ++c;
  }
  std::sort(natives.rbegin(), natives.rend());
  OutputSize best{natives.back(), 1};
  int best_h = 0;
  for (int m : natives)
    for (int scale = 1; scale <= 8; ++scale) {
      const int h = m * scale;
      if (h == height) return {m, scale};
      const bool better = best_h < height ? h > best_h : (h >= height && h < best_h);
      if (better) best = {m, scale}, best_h = h;
    }
  return best;
}

}  // namespace

void SetOutput(trg::SettingsStore& st, const GameInfo& game, int width, int height, bool only_if_missing) {
  if (only_if_missing) {
    if (!st.Get("video_mode_height").empty()) return;
    if (!st.Get("window_height").empty()) {  // a window size chosen before this row existed is kept
      width = st.GetInt("window_width", 0);
      height = st.GetInt("window_height", 0);
    }
  }
  int out_h = height;
  if (out_h == 0) {
    SDL_DisplayMode dm{};
    out_h = SDL_GetDesktopDisplayMode(0, &dm) == 0 ? dm.h : 1080;
  }
  const OutputSize o = OutputFor(game, out_h);
  st.SetInt("window_width", width);
  st.SetInt("window_height", height);
  st.SetInt("video_mode_width", o.mode_height * 16 / 9);
  st.SetInt("video_mode_height", o.mode_height);
  st.SetInt("resolution_scale", o.scale);
  st.Save();
}

std::string GameDir(const GameInfo& game) { return LauncherDir() + "/games/" + game.id; }
std::string GameDataDir(const GameInfo& game) { return GameDir(game) + "/game"; }
#if defined(_WIN32)
std::string GameExePath(const GameInfo& game) {
  // A PlayStation port has a build per disc: the installed disc's, else the first (the download's marker).
  if (IsPsx(game) && !game.psx_discs.empty()) {
    const InstalledPsx installed = InstalledPsxDisc(game);
    return PsxExePath(game, installed.disc ? *installed.disc : game.psx_discs.front());
  }
  return GameDir(game) + "/" + game.exe;
}
std::string GameSettingsPath(const GameInfo& game) { return GameDir(game) + "/" + game.id + ".toml"; }
#else
// The games come inside the AppImage, beside the launcher, without ".exe"; each reads its settings from its user
// folder (the games' OnConfigurePaths).
std::string GameExePath(const GameInfo& game) {
  // A PlayStation build keeps its settings and files beside itself, so it cannot run inside the AppImage: the
  // AppRun copies the builds to games/<id> (LauncherDir() is the user's data folder here).
  if (IsPsx(game) && !game.psx_discs.empty()) {
    const InstalledPsx installed = InstalledPsxDisc(game);
    return PsxExePath(game, installed.disc ? *installed.disc : game.psx_discs.front());
  }
  std::string exe = game.exe;
  if (exe.size() > 4 && exe.compare(exe.size() - 4, 4, ".exe") == 0) exe.resize(exe.size() - 4);
  return ProgramDir() + "/" + exe;
}
std::string GameSettingsPath(const GameInfo& game) { return SavesDir(game) + "/" + game.id + ".toml"; }
#endif
std::string BackupsDir(const GameInfo& game) { return LauncherDir() + "/save-backups/" + game.id; }
std::string CacheDir() { return LauncherDir() + "/cache"; }

std::string SavesDir(const GameInfo& game) {
  // PlayStation memory cards: beside the launcher, outside games/<id> so removing the game keeps them.
  if (IsPsx(game)) return LauncherDir() + "/saves/" + game.id;
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
  if (IsPsx(game)) return InstalledPsxDisc(game).disc ? GameState::kInstalled : GameState::kDownloaded;
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
