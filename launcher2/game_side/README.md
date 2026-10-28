# Game side: what each game needs for TRG Launcher 2.0

The launcher writes settings and starts the games; small hooks inside each game read those settings and talk back.
These files are the ones The Whopping Recompilation Collection ships in its three ReXGlue games. Copy the ones you
need into your game's `src` and add them to its CMake sources.

All of them use `bk_` as their setting prefix. Rename it to your project's `setting_prefix` (search and replace
`bk_` in the files you copy), and set the launcher's `from_launcher_flag` to match the renamed `bk_from_launcher`.

## The contract

| What | Launcher side | Game side |
| --- | --- | --- |
| Settings | writes `games/<id>/<id>.toml` (Linux: `<user data>/<id>.toml`) before Play | the ReXGlue runtime reads `<exe folder>/<app name>.toml` at start; the app name must equal the game's `id` |
| Started by the launcher | passes `from_launcher_flag` (`--bk_from_launcher=true`) and `--game_data_root=games/<id>/game` | `return_to_launcher.cpp` defines the flag |
| Back to the launcher | minimises while the game runs, comes back when the process ends | holding Back and Start for a second, or Esc for a second and a half, closes the game |
| Window to the front | calls `AllowSetForegroundWindow` for the new process | `game_window.cpp` brings its window forward once and hides the cursor while the controller is used |
| Achievements page | reads `games/<id>/game/achievements/achievements.toml`, its icons, and the runtime's unlock file | `achievements_export.cpp` writes the list and icons out of the game's XDBF on its first run |
| Test runs | `test_background_env` set: nothing takes focus | the same variable (`BK_TEST_BACKGROUND`) keeps the game window where it opens |

## Shared files (`shared/`)

| File | Settings it reads | Notes |
| --- | --- | --- |
| `return_to_launcher.cpp` | `bk_from_launcher` | called from the game's XInputGetState hook, so it needs an input hook (see `input_options.cpp`) |
| `game_window.cpp` | `bk_from_launcher` | Windows only; does nothing on Linux, where the desktop brings new windows forward |
| `render_quality.cpp` | `bk_render_quality` | upscaler modes pick a whole number render scale from the resolution chosen in the launcher (Features::output_resolution), or the screen height for Your screen (Features::render_quality) |
| `achievement_toast.cpp` | `bk_achievement_toasts`, `bk_achievement_sound`, `bk_achievement_volume` | the in game pop up and a chime made in code (Features::achievement_toasts) |
| `achievements_export.cpp` | none | needed for the launcher's Achievements section to list anything |

## Examples from Sneak King (`sneak_king_examples/`)

These show how a game wires the settings in. Every guest address in them (`sub_82...`, `0x82...`) is Sneak King's own:
find the matching function in your game before using any of it.

| File | Settings it reads | What to find in your game |
| --- | --- | --- |
| `input_options.cpp` | `bk_invert_x`, `bk_invert_y`, `bk_stick_sensitivity`, `bk_deadzone`, `bk_vibration`, `bk_remap_<button>` | the title's XInputGetState and XInputSetState wrappers (Features::controller_options). Keep its lock around every Xam input call, and hook any other wrapper that calls one (XInputGetKeystroke): the runtime's InputSystem is not safe to call from two threads at once |
| `frame_stats.cpp` | `bk_frame_rate`, `bk_log_fps` | the one function that calls `VdSwap`, where the game paces itself (Features::frame_rate) |
| `mouse_look.cpp` | `bk_mouse_sensitivity`, `bk_mouse_invert_y` | the camera's stick turn (GameInfo::own_mouse_look). The playbook's Sneak King page explains how it was found |
| `port_defaults.cpp` | none | the port's own cvar defaults, such as `readback_resolve` and the Linux render target path |

The keys without a prefix that the launcher writes (`fullscreen`, `present_effect`, `resolution_scale`,
`swap_post_effect`, `anisotropic_override`, `mnk_mode`, `mnk_mouse`, `mnk_sensitivity`, the key bindings,
`audio_mute`, VSync's `d3d12_allow_variable_refresh_rate_and_tearing` and `vulkan_allow_present_mode_immediate`) are
the ReXGlue runtime's own and need nothing in the game.

## Linux

On Linux each game overrides `OnConfigurePaths` so its settings file and logs live in its user data folder
(`XDG_DATA_HOME/<id>`), because the AppImage is read only. The launcher already writes there.
