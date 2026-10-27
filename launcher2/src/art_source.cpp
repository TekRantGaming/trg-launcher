// Where the game art is downloaded from: SteamGridDB's image server
// (cdn2.steamgriddb.com), which serves each image to anyone without an API key.
// Each game's images are picked on its SteamGridDB page and named in its
// GameInfo (art_grid, art_hero, art_logo: the part of the image's address after
// cdn2.steamgriddb.com/). The result is cached (art.h), so each image is
// downloaded once. See launcher2/README.md, "Game art".

#include <string>

#include <trg/download.h>

#include "art.h"
#include "games.h"

namespace trg::hub {

bool g_art_downloads_allowed = true;

std::string FetchArt(int steamgriddb_id, ArtKind kind, std::string& bytes, std::string& ext) {
  bytes.clear();
  if (!g_art_downloads_allowed) return "offline";
  for (const GameInfo& g : Games()) {
    if (g.steamgriddb_id != steamgriddb_id) continue;
    const char* path = kind == ArtKind::kGrid ? g.art_grid : kind == ArtKind::kHero ? g.art_hero : g.art_logo;
    if (!path || !*path) return "no art for this game";
    const std::string p = path;
    const size_t dot = p.rfind('.');
    ext = dot == std::string::npos ? ".img" : p.substr(dot);
    return trg::HttpGet("https://cdn2.steamgriddb.com/" + p, bytes);
  }
  return "no art for this game";
}

}  // namespace trg::hub
