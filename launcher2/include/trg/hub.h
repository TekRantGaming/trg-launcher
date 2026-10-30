// TRG Launcher 2.0: a console style dashboard that launches one or more
// recompiled games. It started as the launcher of The Whopping Recompilation
// Collection and is built on the TRG Launcher 1.x library (settings stores,
// Xbox 360 disc reading, downloads and the updater).
//
// A project fills in one ProjectConfig (its games, names, art, links and
// release names) and calls trg::hub::Run from main. Everything else (home
// screen, game pages, settings, installs from the player's disc image or
// Xbox Live Arcade package, updates, achievements, saves, Add to Steam,
// Discord, Other TRG Projects) comes from the launcher.
// See launcher2/README.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <trg/platform.h>
#include <trg/settings.h>

namespace trg::hub {

// Where a game's files come from: the player's own copy.
enum class InstallSource {
  kDisc,     // an Xbox 360 disc image (.iso); its title ID is checked, then the disc is extracted
  kPackage,  // an Xbox Live Arcade package (the file in Content/<profile>/<title ID>/000D0000), or a folder it was
             // extracted to; the full game is always unlocked by the game's port, not here
  kApk,      // an Android game: the player's own .apk (its package name is checked) and, when the game has one, its
             // main.<version>.<package>.obb expansion file; GameInfo::install_apk turns them into the PC game
};

// What GameInfo::install_apk is given: the player's files and where the game goes (UTF-8 paths).
struct ApkInstall {
  std::string apk;   // the player's .apk
  std::string obb;   // its main.*.obb beside it ("" when the game has none or it was not found)
  std::string dest;  // GameDir: games/<id>
};

struct GameInfo {
  const char* id;          // folder and settings name: "sneak_king" (games/<id>, <id>.toml, Documents/<id>)
  const char* title;       // "Sneak King"
  const char* exe;         // "sneak_king.exe" (also the ReXGlue app name, so its settings file is sneak_king.toml)
  const char* zip_folder;  // the folder inside its release zip, and the zip's name start ("SneakKing")
  uint32_t title_id;       // the Xbox 360 title ID the player's copy must have
  ImVec4 accent;           // the game's colour, used until its art arrives
  const char* tagline;     // one line under the title on its page
  InstallSource source;
  // Art from SteamGridDB's image server: the part after https://cdn2.steamgriddb.com/ ("grid/<hash>.png").
  int steamgriddb_id;      // https://www.steamgriddb.com/game/<id>: art is cached under this number
  const char* art_grid;    // 600 x 900 portrait card
  const char* art_hero;    // 1920 x 620 background
  const char* art_logo;    // transparent title logo
  // The copy the port was made with, shown on the install page and the About section. Only the title ID is checked,
  // so any good copy of the same game works.
  const char* copy_version;  // default.xex's version ("0.0.0.70")
  const char* copy_name;     // the tested image or package's file name
  const char* copy_size;     // "7,027,228,672 bytes"
  const char* copy_sha1;
  const char* copy_md5;
  const char* copy_crc32;
  const char* media_id;      // the execution info's media ID
  const char* installed_size;  // "about 1.6 GB"
  // The game turns its camera with the mouse itself (its own mouse look hook): its Keyboard and mouse section shows
  // <prefix>mouse_sensitivity and <prefix>mouse_invert_y instead of the runtime's mouse to stick sensitivity.
  bool own_mouse_look = false;

  // The file (relative to games/<id>) whose presence means the player's copy is installed. nullptr: the Xbox 360
  // layout, "game/default.xex".
  const char* installed_marker = nullptr;
  // kApk: the Android package name the player's .apk must have ("com.fde.avpevolution"), and whether the game also
  // needs its OBB expansion file.
  const char* android_package = nullptr;
  bool needs_obb = false;
  const char* copy_obb = nullptr;  // kApk: the tested OBB's file name and size, for the install page
  // kApk: builds the PC game from the player's files into ApkInstall::dest, reporting progress on the task. Returns
  // "" on success or a message for the player. Runs on a worker thread.
  std::function<std::string(trg::Task&, const ApkInstall&)> install_apk;
};

// The rows a project's own settings sections draw (ProjectConfig::game_settings). Keys are read and written in the
// game's settings file (games/<id>/<id>.toml) as they change, like the launcher's own rows.
class SettingsRows {
 public:
  using Options = std::vector<std::pair<std::string, std::string>>;  // {value, label}
  virtual ~SettingsRows() = default;
  virtual void Heading(const std::string& text) = 0;
  virtual void Paragraph(const std::string& text) = 0;
  virtual void Info(const std::string& label, const std::string& value) = 0;
  virtual bool Button(const std::string& label, const std::string& help, const std::string& value = "") = 0;
  virtual bool Toggle(const std::string& label, const std::string& help, const std::string& key, bool def) = 0;
  virtual bool Choice(const std::string& label, const std::string& help, const std::string& key,
                      const std::string& def, const Options& options) = 0;
  virtual bool SliderInt(const std::string& label, const std::string& help, const std::string& key, int def, int min,
                         int max, int step, const char* fmt = "%d") = 0;
  virtual bool SliderFloat(const std::string& label, const std::string& help, const std::string& key, float def,
                           float min, float max, float step, const char* fmt = "%.1f") = 0;
  virtual trg::SettingsStore& store() = 0;  // the game's settings file, for presets that set several keys
};

struct SettingsSection {
  std::string name;
  std::function<void(SettingsRows&)> draw;
};

// A tile on the Other TRG Projects page. Art links are full URLs (usually SteamGridDB); "" draws a plain card.
struct OtherProject {
  const char* name;
  const char* owner;
  const char* repo;
  const char* grid_url;   // wide art for the tile
  const char* hero_url;   // background when focused ("" when there is none)
  bool installable = false;  // King Kong Recompiled only: downloads and starts from the page (Windows)
};

// The TekRantGaming ports as listed on King Kong Recompiled's Workshop page, with their art.
const std::vector<OtherProject>& TrgProjects();

// Settings rows that need the matching game side hooks (launcher2/game_side). Turn a feature off when the
// project's games do not have its hook, and its rows disappear.
struct Features {
  bool frame_rate = true;          // <prefix>frame_rate: the game paces itself (frame_stats.cpp)
  bool render_quality = true;      // <prefix>render_quality: upscaler modes (render_quality.cpp)
  bool controller_options = true;  // invert, camera sensitivity, deadzone, vibration, button remapping (input hook)
  bool achievement_toasts = true;  // the in game pop up and chime (achievement_toast.cpp)
  bool achievements = true;        // the Achievements section (the game writes the ReXGlue achievement files)
};

struct ProjectConfig {
  // Names.
  std::string name;     // "The Whopping Recompilation Collection": window title, Steam shortcut, About
  std::string id;       // "whopping": the launcher's own settings file (<id>.txt) and temporary file names
  std::string version;  // "1.0.6": shown in the launcher and compared with releases by the self updater
  std::string home_line;  // under the logo on Home ("All 3 games in one collection"); "" for none
  std::string about;      // the About section's paragraph
  std::string made_by;    // install page: "Blitz Games for King Games, 2006"
  std::string console = "Xbox 360";
  std::string region;     // install page: "USA (NTSC)"; "" hides the row
  // The project's logo (PNG bytes), drawn at the top of Home. Make the header with tools/png_to_header.py.
  const unsigned char* logo_png = nullptr;
  size_t logo_png_size = 0;

  std::vector<GameInfo> games;

  // Releases (docs: two releases per version, see launcher2/README.md).
  std::string repo;                               // "TekRantGaming/whopping-recompilation-collection"
  std::string packages_tag_prefix = "packages-v";  // the release the launcher downloads from
  std::string launcher_zip;   // "WhoppingCollection": its update is <zip>-<version>-windows-x64.zip (Windows) or
                              // <zip>-<version>-linux-x86_64.AppImage (Linux)
  std::string launcher_exe;   // "whopping_launcher" (".exe" is added on Windows)
  std::string linux_data_folder;  // "whopping_collection": XDG_DATA_HOME/<this> holds the launcher's data on Linux

  // The contract with the games (launcher2/game_side).
  std::string setting_prefix = "port_";  // the games' own setting names start with this ("bk_frame_rate")
  std::string from_launcher_flag;        // passed to each game: "--bk_from_launcher=true" (return_to_launcher.cpp)
  std::string test_background_env;       // when set, windows stay where they open (test runs): "BK_TEST_BACKGROUND"
  std::string release_server_env;        // a fake release server for tests: "WHOPPING_RELEASE_SERVER"
  Features features;

  // A game's own settings sections. When set, they replace the ReXGlue runtime's sections (Display, Graphics,
  // Controller, Keyboard and mouse, Audio) for that game; Achievements, Saves and install, Updates and About stay.
  // Empty (the default): the ReXGlue sections.
  std::function<std::vector<SettingsSection>(int game)> game_settings;
  // Words for the About section and the install page ("an Android port", "static recompilation of the Xbox 360
  // game"): how the PC version was made. "" keeps the recompilation wording.
  std::string port_kind;

  // Links. An empty value hides its tile.
  std::string discord_url;     // "https://discord.gg/..."
  std::string steam_art_base;  // Add to Steam artwork folder URL, ending in "/": cover.png, wide.png, hero.png,
                               // logo.png and icon.png (raw.githubusercontent.com/<repo>/main/docs/steam/)
  std::vector<OtherProject> other_projects = TrgProjects();
};

// Runs the launcher until the player quits. Call once from main.
int Run(const ProjectConfig& config, int argc, char** argv);

// The project given to Run.
const ProjectConfig& Project();

}  // namespace trg::hub
