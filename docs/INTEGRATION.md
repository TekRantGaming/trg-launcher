# Integrating TRG Launcher into a port

This guide covers adding the launcher to a game. [examples/demo/main.cpp](../examples/demo/main.cpp) is a working
launcher that you can copy as a starting point.

## 1. Add the repo

```bash
git submodule add https://github.com/TekRantGaming/trg-launcher.git third_party/trg-launcher
```

Pin a tag in `.gitmodules` (`branch = main`, then check out a release tag) so that launcher changes reach a game only
when you choose to update it.

## 2. CMake

```cmake
# Optional, before add_subdirectory:
set(TRG_LAUNCHER_IMGUI_TARGET my_imgui)     # reuse the game's Dear ImGui (1.92+)
set(TRG_LAUNCHER_STANDALONE ON)             # build the SDL2 + OpenGL 3 window
set(TRG_LAUNCHER_IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)   # where imgui_impl_sdl2.cpp lives
add_subdirectory(third_party/trg-launcher)

target_link_libraries(my_game PRIVATE trg::launcher_standalone)    # or trg::launcher
```

| Option | Default | Meaning |
| --- | --- | --- |
| `TRG_LAUNCHER_IMGUI_TARGET` | *(empty)* | The game's ImGui target. If empty, the target `imgui` is used when it exists, otherwise ImGui is downloaded. |
| `TRG_LAUNCHER_STANDALONE` | ON at the top level, OFF as a subproject | Builds `trg::launcher_standalone` (SDL2 + OpenGL 3). |
| `TRG_LAUNCHER_IMGUI_DIR` | the downloaded copy | Folder containing `imgui_impl_sdl2.cpp` / `imgui_impl_opengl3.cpp`, either at its root or in `backends/`. |
| `TRG_LAUNCHER_IMGUI_HAS_BACKENDS` | OFF | Set this when the game's ImGui target already compiles those two backends. Otherwise their symbols are defined twice. |
| `TRG_LAUNCHER_FETCH_DEPS` | ON | Allows downloading ImGui and SDL2 when the game provides neither. |
| `TRG_LAUNCHER_BUILD_DEMO` | ON at the top level | Builds the demo. |

SDL2 comes from an existing `SDL2::SDL2` / `SDL2::SDL2-static` target, from `find_package(SDL2)`, or is downloaded as
a static library.

## 3. Pick a host

### Standalone window (the game has no ImGui window yet)

This is the SMS case. The launcher runs before the game creates its own window:

```cpp
int main(int argc, char** argv) {
  trg::TextFileSettings settings(base_dir + "settings.txt");
  const bool ready = GameIsInstalled(settings);
  if (trg::ShouldShowLauncher(settings, "launcher", ready, HasFlag(argc, argv, "--launcher"))) {
    trg::Launcher launcher(MakeLauncherConfig(settings));
    if (trg::RunStandalone(launcher, {"Super Mario Sunshine - Launcher"}) != trg::Result::kPlay) return 0;
    settings.Load();  // pick up what the launcher wrote
  }
  return RunGame();
}
```

`RunStandalone` creates and destroys its own ImGui context, initialises SDL's video subsystem and shuts it down again
afterwards. The game can then open its own window as normal. If no OpenGL 3.3 window can be opened, it returns `kPlay`
so the game still starts.

Textures for the icon or header art need the window, so create them in `StandaloneHooks::on_start` with
`trg::CreateTextureRGBA()` and free them in `on_stop`.

### Embedded (the game already draws ImGui)

This is the OKX / ReXGlue case. Call `Frame()` from the game's ImGui overlay until the player presses Play:

```cpp
class LauncherDialog final : public rex::ui::ImGuiDialog {
 public:
  LauncherDialog(rex::ui::ImGuiDrawer* drawer, trg::LauncherConfig config, std::function<void()> play,
                 std::function<void()> quit)
      : ImGuiDialog(drawer), launcher_(std::move(config)), play_(std::move(play)), quit_(std::move(quit)) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    switch (launcher_.Frame()) {
      case trg::Result::kPlay: Close(); play_(); break;
      case trg::Result::kQuit: quit_(); break;
      case trg::Result::kNone: break;
    }
  }

 private:
  trg::Launcher launcher_;
  std::function<void()> play_, quit_;
};
```

`Frame()` applies its theme and restores the game's `ImGuiStyle` before returning, so the game's own overlays keep
their look.

Fonts: pass the game's fonts in `config.fonts` (`regular`, `semibold`, `bold`, plus the `size` they were designed
for), or call `trg::LoadSystemFonts()` once before the first frame.

## 4. Settings

The launcher reads and writes only through a `trg::SettingsStore`.

**Plain text file** (`name = value`, as in SMS):

```cpp
trg::TextFileSettings settings("settings.txt");
```

This store keeps comments and order. A setting that was not in the file replaces its commented example
(`# vsync = on`) when the file has one; otherwise it is appended under `# Set from the launcher`.

**Engine cvars** (OKX / ReXGlue):

```cpp
trg::CallbackSettings settings;
settings.true_value = "true";   // what toggles write
settings.false_value = "false";
settings.get = [](std::string_view k) { return rex::cvar::GetFlagByName(std::string(k)); };
settings.set = [](std::string_view k, const std::string& v) { rex::cvar::SetFlagByName(std::string(k), v); };
settings.save = [path] { return okx::SaveSettings(path); };
settings.reset = [] { /* rex::cvar::ResetToDefault for each non-command cvar */ };
```

Toggles accept `on/off`, `true/false`, `yes/no` and `1/0` when reading. They write `true_value` / `false_value`.

Settings are saved when the player presses Play (`config.save_on_play`) or Save. Use `config.on_save` to write extra
files, such as a key-bindings file.

## 5. Pages

```cpp
config.pages = {
  {"Install", "Point the launcher at your own disc image to install the game.",
   [&](trg::Ui& ui) { InstallPage(ui); },
   [&] { return !GameIsInstalled(); }},   // orange dot in the sidebar
  {"Display", "Window, monitor and how the picture fits your screen.", DisplayPage},
  ...
};
```

Widgets, all on `trg::Ui`:

| Widget | Stored as |
| --- | --- |
| `Choice(label, help, key, def, options, max_buttons = 4)` | Segmented buttons. More options than `max_buttons` becomes a combo. |
| `Combo(...)` | Always a combo |
| `Toggle(label, help, key, def, "Off", "On")` | `true_value` / `false_value` |
| `SliderInt(..., lo, hi, "%d%%", step)` / `SliderFloat(...)` | number |
| `InputText(label, help, key, hint)` | text |
| `KeyBindRow(label, help, shown)` | Returns the `ImGuiKey` pressed. Store it in the game's own key names, using `ImGui::GetKeyName()` or `trg::ImGuiKeyToVirtualKey()`. |
| `Info`, `ButtonRow`, `ConfirmRow`, `FolderRow`, `LauncherVisibilityRow`, `ResetAllRow` | Ready-made rows |
| `StatusCard`, `TaskProgress`, `Heading`, `Paragraph`, `Help`, `Section`, `Spacer` | Non-row content |
| `Row(label, help)` + any ImGui widgets | Custom controls in the right-hand column |

Rows share a two-column table that opens automatically. Any non-row widget closes it, and the next row opens a new one.

Values that only apply on restart, such as the window size in OKX, are the game's responsibility. Compare them before
and after the launcher runs.

## 6. Install pages

```cpp
void InstallPage(trg::Ui& ui) {
  std::string error;
  switch (install_task.Finish(&error)) {   // once, when the job ends
    case trg::Task::State::kDone: ui.SetStatus("Installed. Press Play to start the game.", 6); break;
    case trg::Task::State::kFailed: ui.SetStatus("Install failed: " + error, 8); break;
    default: break;
  }
  if (install_task.running())
    ui.StatusCard(trg::Status::kBusy, "Installing...", nullptr, std::max(0.0f, install_task.fraction()));
  else if (GameIsInstalled())
    ui.StatusCard(trg::Status::kReady, "Ready to play", image_path.c_str());
  else
    ui.StatusCard(trg::Status::kAttention, "Game not installed", "Select your disc image below.");

  ui.Row("Disc image", "Your own disc, as an ISO. You can also drop the file onto this window.");
  if (ui.AccentButton("Browse...")) picked = trg::BrowseForFile("Select your disc image", {{"Disc images", "*.iso"}});
  ui.EndRows();
  if (!ui.TaskProgress(install_task) && ui.AccentButton("Install"))
    install_task.Start("Copying", [=](trg::Task& t) { return trg::CopyFileJob(t, picked, dest); });
}
```

To block Play until the game is installed:

```cpp
config.can_play = [&] {
  if (install_task.running()) return trg::PlayCheck{false, "Wait for the install to finish.", 0};
  if (!GameIsInstalled()) return trg::PlayCheck{false, "Install the game first.", 0};
  return trg::PlayCheck{};
};
config.on_file_drop = [&](const std::string& path) { picked = path; launcher->GoToPage(0); };
```

A `Task` job runs on a worker thread. It reports progress with `t.Progress(done, total)`, checks `t.cancelled()`,
and returns `""` on success or an error message on failure.

## 7. Look

```cpp
config.theme = trg::Theme::Midnight().WithAccent(ImVec4(0.30f, 0.65f, 1.0f, 1.0f));
config.branding.title = "GAME TITLE";
config.branding.subtitle = "PC PORT   \xC2\xB7   LAUNCHER";
config.branding.background = trg::headers::Space({ImVec4(0.3f, 0.5f, 0.9f, 1)});  // planet colour
config.branding.icon = icon_texture;
```

A custom header is any function that takes a `trg::HeaderContext`:

```cpp
config.branding.background = [](const trg::HeaderContext& c) {
  c.draw->AddRectFilledMultiColor(c.min, c.max, ...);
  // c.time is in seconds (for animation); c.scale is the UI scale
};
```

OKX shows the game's own title screen once it has been captured, and its starfield until then:

```cpp
config.branding.background = trg::headers::Image(&title_texture, &title_aspect, 0.04f, trg::headers::Space());
```

`shade_title_side` and `fade_into_page` darken the area behind the title and blend the header into the page. Turn
both off for bright headers such as the SMS sky.
