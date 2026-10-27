# TRG Launcher: notes for Claude

This repo is the shared launcher for every TekRantGaming PC port. **When a new port needs a launcher, pull this repo
in. Do not write a new one.** It already contains the design built for Outpost Kaloki X and the SMS port: animated
header, sidebar pages, label/help rows with segmented buttons, the status card and the Quit / Save / PLAY footer.

## Two launchers

- **2.0** (`launcher2/`, `trg::launcher2`): the dashboard for one game or a collection, from The Whopping
  Recompilation Collection. A project copies `launcher2/examples/whopping/project.cpp`, fills in its
  `trg::hub::ProjectConfig` and calls `trg::hub::Run`. Read `launcher2/README.md` first. Improvements to the
  dashboard belong in `launcher2/src`, never in a project's copy; anything project specific goes into
  `ProjectConfig` (with a default that keeps existing projects working).
- **1.x** (the rest of this repo): the settings window before one game, described below. 2.0 is built on it.

## Adding 1.x to a new game

1. `git submodule add https://github.com/TekRantGaming/trg-launcher.git third_party/trg-launcher`
2. Pick the host:
   - The game already draws Dear ImGui (ReXGlue titles): link `trg::launcher` and call `Launcher::Frame()` from an
     ImGui dialog.
   - Otherwise: set `TRG_LAUNCHER_STANDALONE ON`, link `trg::launcher_standalone`, and call `trg::RunStandalone()`
     before the game creates its window.
3. Copy `examples/demo/main.cpp` as the starting point, then replace its pages with the game's real settings.
4. Give it a game-specific look: `Theme::Midnight/Ocean/Ember().WithAccent(...)`, plus a header painter (a stock
   scene from `headers.h`, or a custom one that evokes the game).
5. Use the game's real setting keys through a `SettingsStore`: `TextFileSettings` for `key = value` files, or
   `CallbackSettings` for engine cvars.

The full guide is in docs/INTEGRATION.md.

## Changing the launcher itself

- Improvements belong here, not in a game's copy, so every game can pick them up.
- Keep games' existing API calls working. Add parameters with defaults rather than changing signatures.
- Build and check with `build.bat`. Render a page to check how it looks:
  `build\examples\demo\trg_launcher_demo.exe --theme ocean --page 1 --ready --screenshot out.ppm`
  (convert the PPM with Pillow to view it).
- Dear ImGui must be 1.92 or newer (`PushFont(font, size)`). Do not use APIs older than that.
- `windows.h` macros clash with names such as `ReplaceFile`, `CreateWindow` and `DrawText`. Avoid those names in the
  `trg` namespace.

## Layout

- `include/trg/launcher.h`: `Launcher`, `LauncherConfig`, `Ui` widgets, `Theme`, `Branding`
- `include/trg/settings.h`: `SettingsStore`, `TextFileSettings`, `CallbackSettings`, `MemorySettings`
- `include/trg/headers.h`: header scenes (`Space`, `SkyAndSea`, `Image`, `Gradient`, `Layers`)
- `include/trg/platform.h`: file dialogs, `OpenInFileManager`, `ShiftHeld`, `Task` (background jobs), `CopyFileJob`
- `include/trg/standalone.h`: `RunStandalone` (SDL2 + OpenGL 3), `CreateTextureRGBA`
- `include/trg/xbox360.h`: `ReadXbox360TitleId`, `ExtractXbox360Disc` (install from an Xbox 360 disc image)
- `include/trg/download.h`: `HttpGet`, `DownloadFile` (WinHTTP on Windows, curl elsewhere)
- `include/trg/shader_pack.h`: ReXGlue shader packs: `ShaderPackRow`, `InstallShaderPack`, `MergeShaderStorageFile`
- `include/trg/game_helpers.h`: game-side `TuneProcessScheduling`, `InstallCrashReports`, `FrameTimeStats`
- `include/trg/updater.h`: updates from GitHub releases (`UpdaterConfig`, `UpdatesRows`, `InstallUpdate`,
  `CheckProgramFiles`); packaging with `tools/make_update_files.py`; the rules and lessons in docs/UPDATER.md

## Lessons from King Kong Recompiled

- Warn about a risky setting with `LauncherConfig::before_play`, not a popup of the game's own.
- On ReXGlue's Vulkan backend, keep launcher textures alive after the launcher closes (see INTEGRATION.md, section 14).
- A port's FIRST public release must already have the updater (docs/UPDATER.md): an older updater in players' hands
  can never be fixed. Package every release with `tools/make_update_files.py` and attach `SHA256SUMS.txt`.
- `TRG_LAUNCHER_AUTOPLAY=1` presses PLAY by itself: use it to test the game start without clicking.
- Per-game features that patch the game itself (King Kong's button prompts, its render-quality presets) stay in the
  game. Only the parts any port can use belong here.
