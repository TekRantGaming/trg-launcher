// PlayStation games (InstallSource::kPsxDisc): psxrecomp ports, one build per disc (trg/hub.h, PsxDisc).
//   Install  reads the serial from the player's disc image (SYSTEM.CNF's BOOT line), then copies the image (the .cue
//            and every track it names, or a single .bin or .iso with a .cue made for it) into games/<id>/game.
//   Play     starts the build for the installed disc with --disc and --memcard-dir, after writing its settings.toml
//            and input.ini from the launcher's settings for the game.
// From Muppet RaceMania Recompiled (USA and Europe discs).
#pragma once

#include <string>
#include <vector>

#include <trg/platform.h>
#include <trg/settings.h>

#include "games.h"

namespace trg::hub {

// The disc's serial ("SLUS-01237") from a .cue, .bin or .iso; "" when the file is not a PlayStation disc image.
std::string ReadPsxSerial(const std::string& path);

// The size of a disc image: every track a .cue names, or the file itself.
long long PsxImageSize(const std::string& path);

// The disc of `game` with this serial, or nullptr.
const PsxDisc* FindPsxDisc(const GameInfo& game, const std::string& serial);

// What the serial is, in words, when it is not one of the game's discs.
std::string DescribePsxSerial(const std::string& serial);

// The installed disc: the one games/<id>/game holds (disc.txt, written by the install), and its .cue.
struct InstalledPsx {
  const PsxDisc* disc = nullptr;
  std::string cue;  // UTF-8
};
InstalledPsx InstalledPsxDisc(const GameInfo& game);

std::string PsxBuildDir(const GameInfo& game, const PsxDisc& disc);  // games/<id>/<folder>
std::string PsxExePath(const GameInfo& game, const PsxDisc& disc);

// Runs on a Task: checks the image's serial against `game` and copies it into `out_dir`. "" on success.
std::string InstallPsxDisc(trg::Task& task, const GameInfo& game, const std::string& path, const std::string& out_dir);

// Writes the runtime's settings.toml and input.ini in `build_dir` from the launcher's settings for the game (the
// PlayStation settings rows in settings_screen.cpp), keeping every other line the runtime wrote there.
bool WritePsxRuntimeSettings(const GameInfo& game, const trg::SettingsStore& st, const std::string& build_dir);

// Controllers switched on now (the Controller section lists them; Play uses the count for player 2).
std::vector<std::string> ConnectedControllers();

// The launcher setting that holds a game's own row: mod.<feature> or mod.<feature>.<option>.
std::string PsxOptionKey(const PsxOption& o);
// Whether the game shows 16:9: its widescreen row (PsxOption::widescreen) is on.
bool PsxWidescreen(const GameInfo& game, const trg::SettingsStore& st);
// The picture height the game is drawn at and shown (the Resolution row; "screen": the monitor's height).
int PsxOutputHeight(const trg::SettingsStore& st);
// The height the game renders at before the upscaler (the output height divided by the mode's ratio; the output
// height itself with the upscaler off).
int PsxRenderHeight(const trg::SettingsStore& st);

}  // namespace trg::hub
