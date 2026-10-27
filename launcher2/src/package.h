// The player's copy of a game: an Xbox Live Arcade package (the one file in Content\<profile>\<title ID>\000D0000),
// a folder holding it, or a folder the package was already extracted to (with default.xex in it).
#pragma once

#include <cstdint>
#include <string>

#include <trg/platform.h>

namespace trg::hub {

// The title ID in an STFS package's header (LIVE, PIRS or CON), or 0 when the file is not one.
uint32_t ReadPackageTitleId(const std::string& utf8_file);
// The title ID in a default.xex (its execution info header), or 0.
uint32_t ReadXexTitleId(const std::string& utf8_xex);

struct GameSource {
  std::string path;       // the package file, or the extracted folder (UTF-8)
  bool extracted = false;
  uint32_t title_id = 0;  // 0 when nothing usable was found
};

// What the player picked, as something to install for title `want`: the package itself, a folder holding the
// package anywhere a few folders down, or an extracted folder. When only another game's copy is found, its title
// ID is returned so the message can say which game it is.
GameSource FindGameSource(const std::string& utf8_path, uint32_t want);

// Extracts the package (or copies the extracted folder) into `out_dir`. Returns "" on success, else an error.
std::string InstallGameSource(trg::Task& task, const GameSource& source, const std::string& out_dir);

}  // namespace trg::hub
