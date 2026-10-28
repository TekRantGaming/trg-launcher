// The project's games (trg/hub.h) and where each one lives on disk.
#pragma once

#include <vector>
#include <cstdint>
#include <string>

#include <trg/settings.h>

#include <trg/hub.h>

namespace trg::hub {

// Each version of a project has two releases: vX, the one players see, with only the portable launcher's zip (and
// the Linux AppImage), and <packages_tag_prefix>X with what the launcher downloads: its own update zip, each game's
// <zip_folder>-X-windows-x64.zip and SHA256SUMS.txt. See launcher2/README.md.

enum class GameState {
  kNotDownloaded,  // no program files yet: the first button says Download
  kDownloaded,     // program files, but no game files from a disc: Install
  kInstalled,      // ready: Play
};

const std::vector<GameInfo>& Games();  // Project().games
int GameCount();

// Folders (UTF-8, forward slashes). Everything lives beside the launcher, so the
// collection can be moved or deleted as one folder; saves stay where the games
// keep them (Documents/<id>).
std::string ProgramDir();   // the launcher's own folder (inside the AppImage on Linux)
std::string LauncherDir();  // where it writes: its folder on Windows, XDG_DATA_HOME/<linux_data_folder> on Linux
// Features::output_resolution: writes the window size and the game's video mode and draw scale for a picture
// width x height (0 x 0: the screen's size), or, with `only_if_missing`, the default 1920 x 1080 when none is set yet.
void SetOutput(trg::SettingsStore& st, const GameInfo& game, int width, int height, bool only_if_missing = false);
std::string GameDir(const GameInfo& game);       // games/<id>: the program files from the release
std::string GameDataDir(const GameInfo& game);   // games/<id>/game: the files from the player's copy
std::string GameExePath(const GameInfo& game);
std::string GameSettingsPath(const GameInfo& game);  // games/<id>/<id>.toml, read by the game at start
std::string SavesDir(const GameInfo& game);      // Documents/<id>
std::string BackupsDir(const GameInfo& game);    // save-backups/<id>
std::string CacheDir();                          // cache (art)

GameState DetectState(const GameInfo& game);
std::string InstalledVersion(const GameInfo& game);  // from update-files.txt; "" when unknown

}  // namespace trg::hub
