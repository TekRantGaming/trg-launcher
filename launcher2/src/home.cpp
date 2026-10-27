// The home screen, after the Xbox Series dashboard: the focused game's art
// fills the window, its logo and status sit on the left, and the games are a
// row of tiles along the bottom, then Other TRG Projects (one more "game" that
// opens the other TekRantGaming projects), Add to Steam, Discord, Settings and
// Exit. Other TRG Projects, Add to Steam and Discord show only when the
// project gives them something (trg/hub.h).
//
// A opens the focused game's page (the only place with Play); LB and RB move between the
// games. The mouse focuses a tile by pointing at it and clicking does what A does.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app.h"
#include "sounds.h"

namespace trg::hub {
namespace {

using namespace dash;

// The row of tiles: the games, then Other TRG Projects, then the square buttons.
enum class TileKind { kGame, kProjects, kSteam, kDiscord, kSettings, kExit };

struct HomeState {
  std::vector<TileKind> tiles;  // index = tile; games first, so a game's tile index is the game's index
  int big = 0;    // tiles drawn as cards: the games and Other TRG Projects (they come first)
  int focus = 0;  // tile with the focus
  int shown = 0;  // whose art and text are shown: a card tile (the last focused of them)
  std::vector<float> fade, grow;
};
HomeState home;

void BuildTiles() {
  if (!home.tiles.empty()) return;
  const ProjectConfig& p = Project();
  for (int i = 0; i < GameCount(); ++i) home.tiles.push_back(TileKind::kGame);
  if (!p.other_projects.empty()) home.tiles.push_back(TileKind::kProjects);
  home.big = int(home.tiles.size());
  if (!p.steam_art_base.empty()) home.tiles.push_back(TileKind::kSteam);
  if (!p.discord_url.empty()) home.tiles.push_back(TileKind::kDiscord);
  home.tiles.push_back(TileKind::kSettings);
  home.tiles.push_back(TileKind::kExit);
  home.fade.assign(size_t(home.big), 0.0f);
  home.fade[0] = 1.0f;
  home.grow.assign(home.tiles.size(), 0.0f);
  home.grow[0] = 1.0f;
}

int ProjectsTile() { return GameCount() < home.big ? GameCount() : -1; }

const char* kBullet = "  \xC2\xB7  ";

std::string Percent(float f) {
  if (f < 0) return "";
  char b[16];
  std::snprintf(b, sizeof b, "  %.0f%%", double(f) * 100.0);
  return b;
}

// The small label on a tile: what is happening to the game.
std::string Badge(App& app, int i) {
  GameUi& gu = app.game_ui(i);
  if (gu.jobs.task.running()) {
    switch (gu.jobs.kind) {
      case GameJobs::Kind::kInstall: return "Installing" + Percent(gu.jobs.task.fraction());
      case GameJobs::Kind::kUpdateCheck: return "Checking";
      default: return "Downloading" + Percent(gu.jobs.task.fraction());
    }
  }
  if (app.running_game() == i) return "Playing";
  switch (app.state(i)) {
    case GameState::kNotDownloaded: return "Not installed";
    case GameState::kDownloaded:
      return Games()[i].source == InstallSource::kDisc ? "Needs your disc" : "Needs your copy";
    default: return "";
  }
}

void DrawTile(App& app, int i, ImVec2 a, ImVec2 b, bool focused) {
  const Frame& f = F();
  ImDrawList* dl = f.dl;
  dl->AddRectFilled(ImVec2(a.x + 4 * f.s, a.y + 8 * f.s), ImVec2(b.x + 4 * f.s, b.y + 8 * f.s), Black(0.45f),
                    10 * f.s);
  const Texture& grid = app.art().Get(i, ArtKind::kGrid);
  if (grid) {
    Cover(grid, a, b, 1.0f, 0.5f, 8 * f.s);
  } else {
    dl->AddRectFilled(a, b, Accent(Games()[i].accent, 0.8f), 8 * f.s);
    Text(f.bold, 20, ImVec2(a.x + 12 * f.s, a.y + 14 * f.s), White(), Games()[i].title, (b.x - a.x) / f.s - 24);
  }
  const std::string badge = Badge(app, i);
  if (!badge.empty()) {
    const ImVec2 t = TextSize(f.semibold, 12, badge);
    const ImVec2 ba(a.x + 8 * f.s, b.y - 8 * f.s - t.y - 6 * f.s), bb(ba.x + t.x + 14 * f.s, b.y - 8 * f.s);
    dl->AddRectFilled(ba, bb, Black(0.75f), 4 * f.s);
    Text(f.semibold, 12, ImVec2(ba.x + 7 * f.s, ba.y + 3 * f.s), White(), badge);
  }
  if (focused) FocusRing(a, b, 8);
}

// Settings, Exit, and the Steam and Discord buttons: those two show their brand's mark on its colour.
void DrawUtilityTile(ImVec2 a, ImVec2 b, const std::string& label, int icon, bool focused, const Texture* mark = nullptr,
                     ImU32 colour = 0) {
  const Frame& f = F();
  ImDrawList* dl = f.dl;
  if (colour) {
    dl->AddRectFilled(a, b, colour, 8 * f.s);
    if (Hover(a, b)) dl->AddRectFilled(a, b, White(0.08f), 8 * f.s);
  } else {
    dl->AddRectFilled(a, b, Hover(a, b) ? White(0.16f) : White(0.09f), 8 * f.s);
  }
  const ImVec2 c((a.x + b.x) * 0.5f, a.y + (b.y - a.y) * 0.42f);
  const float r = 14 * f.s, w = 1.8f * f.s;
  if (mark && *mark) {
    const float m = 17 * f.s;
    dl->AddImage(ImTextureRef(mark->id), ImVec2(c.x - m, c.y - m), ImVec2(c.x + m, c.y + m));
  } else if (icon == 0) {  // settings: three sliders
    for (int k = -1; k <= 1; ++k) {
      const float y = c.y + k * 9 * f.s;
      dl->AddLine(ImVec2(c.x - r, y), ImVec2(c.x + r, y), White(0.85f), w);
      dl->AddCircleFilled(ImVec2(c.x + (k == 0 ? 6 : -6) * f.s, y), 3.5f * f.s, White(), 12);
    }
  } else {  // exit: a power symbol
    dl->PathArcTo(c, r * 0.85f, -3.14159f * 0.5f + 0.7f, 3.14159f * 1.5f - 0.7f, 24);
    dl->PathStroke(White(0.85f), 0, w);
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y - 2 * f.s), White(0.85f), w);
  }
  const ImVec2 t = TextSize(f.regular, 13, label, (b.x - a.x) / f.s - 12);
  Text(f.regular, 13, ImVec2(c.x - t.x * 0.5f, b.y - (b.y - a.y) * 0.3f), White(0.9f), label, (b.x - a.x) / f.s - 12);
  if (focused) FocusRing(a, b, 8);
}

// Other TRG Projects: the first four projects' wide cards stacked, the name across the bottom.
void DrawProjectsTile(App& app, ImVec2 a, ImVec2 b, bool focused) {
  const Frame& f = F();
  ImDrawList* dl = f.dl;
  dl->AddRectFilled(ImVec2(a.x + 4 * f.s, a.y + 8 * f.s), ImVec2(b.x + 4 * f.s, b.y + 8 * f.s), Black(0.45f),
                    10 * f.s);
  dl->AddRectFilled(a, b, IM_COL32(28, 29, 34, 255), 8 * f.s);
  // The first four with art (for the TRG list: King Kong, Earthworm Jim, Super Mario Sunshine, Simpsons Wrestling).
  std::vector<size_t> picks;
  for (size_t i = 0; i < app.projects().list().size() && picks.size() < 4; ++i)
    if (*app.projects().list()[i].grid_url) picks.push_back(i);
  const int n = std::max(1, int(picks.size()));
  const float gap = 3 * f.s, sh = (b.y - a.y - gap * float(n - 1)) / float(n);
  dl->PushClipRect(a, b, true);
  for (int k = 0; k < int(picks.size()); ++k) {
    const ImVec2 sa(a.x, a.y + k * (sh + gap)), sb(b.x, sa.y + sh);
    FitArt(app.projects().info(picks[size_t(k)]).grid, sa, sb, k == 0 ? 8 * f.s : 0.0f);
  }
  dl->PopClipRect();
  const char* label = "Other TRG Projects";
  const ImVec2 ts = TextSize(f.semibold, 14, label, (b.x - a.x) / f.s - 16);
  const ImVec2 la(a.x, b.y - ts.y - 18 * f.s);
  dl->AddRectFilledMultiColor(ImVec2(a.x, la.y - 20 * f.s), b, Black(0.0f), Black(0.0f), Black(0.92f), Black(0.92f));
  Text(f.semibold, 14, ImVec2(a.x + 8 * f.s, la.y + 6 * f.s), White(), label, (b.x - a.x) / f.s - 16);
  if (focused) FocusRing(a, b, 8);
}

void Activate(App& app, int tile) {
  switch (home.tiles[size_t(tile)]) {
    case TileKind::kGame: app.OpenHub(tile); break;
    case TileKind::kProjects: app.OpenProjects(); break;
    case TileKind::kSteam: app.SteamPressed(); break;
    case TileKind::kDiscord: app.OpenDiscord(); break;
    case TileKind::kSettings: app.OpenCollection(); break;
    case TileKind::kExit: app.Quit(); break;
  }
}

}  // namespace

void FocusGameOnHome(int game) {
  BuildTiles();
  if (game < 0 || game >= GameCount()) return;
  home.focus = home.shown = game;
}

void FocusProjectsOnHome() {
  BuildTiles();
  if (ProjectsTile() >= 0) home.focus = home.shown = ProjectsTile();
}

void DrawHome(App& app) {
  const Frame& f = F();
  const float dt = std::min(0.1f, ImGui::GetIO().DeltaTime);
  const bool still = app.options().frames < 60;  // screenshots: no animation

  BuildTiles();
  const int tiles = int(home.tiles.size()), big = home.big;

  // Input.
  if (Pressed(Btn::kLeft)) home.focus = std::max(0, home.focus - 1);
  if (Pressed(Btn::kRight)) home.focus = std::min(tiles - 1, home.focus + 1);
  // LB and RB move between the card tiles (the games and Other TRG Projects).
  if (Pressed(Btn::kLB)) home.focus = (std::min(home.focus, big - 1) + big - 1) % big;
  if (Pressed(Btn::kRB)) home.focus = home.focus >= big - 1 ? 0 : home.focus + 1;
  if (home.focus < big) home.shown = home.focus;
  const bool on_game = home.focus < big;
  const bool on_projects = home.shown == ProjectsTile();
  const int g = on_projects ? 0 : home.shown;

  // Art, crossfading to the focused game.
  for (int i = 0; i < big; ++i) {
    const float target = i == home.shown ? 1.0f : 0.0f;
    home.fade[size_t(i)] = still ? target : home.fade[size_t(i)] + (target - home.fade[size_t(i)]) * std::min(1.0f, dt * 7.0f);
  }
  f.dl->AddRectFilled(f.o, f.end, IM_COL32(8, 8, 10, 255));
  for (int i = 0; i < GameCount(); ++i) Backdrop(app.art().Get(i, ArtKind::kHero), home.fade[size_t(i)], 0.72f);
  if (ProjectsTile() >= 0)  // the first project's background (King Kong in the TRG list) for Other TRG Projects
    Backdrop(app.projects().info(0).hero, home.fade[size_t(ProjectsTile())], 0.6f);
  Shade();
  if (TopBar({}, true)) app.OpenCollection();
  // A soft dark band at the top keeps the wordmark, its line and the clock readable over bright art.
  f.dl->AddRectFilledMultiColor(f.o, ImVec2(f.end.x, f.Y(250)), Black(0.72f), Black(0.72f), Black(0.0f), Black(0.0f));
  // The project's wordmark, centred at the top, and its line.
  if (const Texture& logo = app.logo()) {
    const float w = 330 * f.s, h = w * float(logo.height) / float(logo.width);
    const float x = f.o.x + (f.size.x - w) * 0.5f, y = f.Y(4);
    f.dl->AddImage(ImTextureRef(logo.id), ImVec2(x, y), ImVec2(x + w, y + h));
    const std::string& line = Project().home_line;
    const ImVec2 t = TextSize(f.semibold, 14, line);
    Text(f.semibold, 14, ImVec2(f.o.x + (f.size.x - t.x) * 0.5f + 1, y + h + 1 * f.s), Black(0.6f), line);
    Text(f.semibold, 14, ImVec2(f.o.x + (f.size.x - t.x) * 0.5f, y + h), White(0.85f), line);
  }

  // The game's logo, then its status.
  const GameInfo& game = Games()[g];
  GameUi& gu = app.game_ui(g);
  float y = f.Y(96);
  const Texture& logo = app.art().Get(g, ArtKind::kLogo);
  if (on_projects) {
    Text(f.bold, 48, ImVec2(f.X(48), y), White(), "Other TRG Projects");
    y += 70 * f.s;
    bool installable = false;
    for (const OtherProject& p : app.projects().list()) installable = installable || p.installable;
    std::string line = std::to_string(app.projects().list().size()) + " more ports from TekRantGaming.";
#if defined(_WIN32)
    if (installable) line += " King Kong Recompiled installs right here.";
#endif
    Text(f.regular, 16, ImVec2(f.X(48), y), White(0.72f), line, 560);
  } else if (logo) {
    float w = 320 * f.s, h = w * float(logo.height) / float(logo.width);
    if (h > 170 * f.s) {
      h = 170 * f.s;
      w = h * float(logo.width) / float(logo.height);
    }
    f.dl->AddImage(ImTextureRef(logo.id), ImVec2(f.X(48), y), ImVec2(f.X(48) + w, y + h));
    y += h + 22 * f.s;
  } else {
    Text(f.bold, 54, ImVec2(f.X(48), y), White(), game.title);
    y += 76 * f.s;
  }
  if (on_projects) {
  } else if (gu.jobs.task.running()) {
    const float fr = gu.jobs.task.fraction();
    Text(f.regular, 16, ImVec2(f.X(48), y), White(0.85f), gu.jobs.task.label() + Percent(fr));
    Bar(ImVec2(f.X(48), y + 30 * f.s), ImVec2(f.X(48 + 380), y + 36 * f.s), fr, White());
  } else if (gu.jobs.message_is_error && !gu.jobs.message.empty()) {
    Text(f.regular, 16, ImVec2(f.X(48), y), IM_COL32(255, 130, 120, 255), gu.jobs.message, 560);
  } else {
    std::string line;
    switch (app.state(g)) {
      case GameState::kNotDownloaded: line = "Not on this PC yet"; break;
      case GameState::kDownloaded:
        line = std::string("Ready") + kBullet + "Play asks for your " +
               (game.source == InstallSource::kDisc ? "disc image" : "copy of the game") + " the first time";
        break;
      case GameState::kInstalled: {
        line = app.running_game() == g ? "Playing now" : "Ready to play";
        const std::string v = InstalledVersion(game);
        if (!v.empty()) line += kBullet + ("Version " + v);
        GameAchievements& ach = app.achievements(g);
        if (ach.list_found)
          line += kBullet + std::to_string(ach.unlocked) + " of " + std::to_string(ach.cards.size()) + " achievements";
        break;
      }
    }
    Text(f.regular, 16, ImVec2(f.X(48), y), White(0.72f), line);
  }

  // Tiles along the bottom: the focused one grows.
  float x = f.X(48);
  const float bottom = f.B(78);
  int hovered = -1;
  for (int t = 0; t < tiles; ++t) {
    const float target = t == home.focus ? 1.0f : 0.0f;
    float& grow = home.grow[size_t(t)];
    grow = still ? target : grow + (target - grow) * std::min(1.0f, dt * 14.0f);
    float w, h;
    if (t < big) {
      w = (132 + 28 * grow) * f.s;
      h = w * 1.5f;
    } else {  // the buttons, square
      if (t == big) x += 22 * f.s;
      w = h = (104 + 10 * grow) * f.s;
    }
    const ImVec2 a(x, bottom - h), b(x + w, bottom);
    const bool focused = t == home.focus;
    switch (home.tiles[size_t(t)]) {
      case TileKind::kGame: DrawTile(app, t, a, b, focused); break;
      case TileKind::kProjects: DrawProjectsTile(app, a, b, focused); break;
      case TileKind::kSteam:
        DrawUtilityTile(a, b, app.SteamLabel(), -1, focused, &app.steam_icon(), IM_COL32(27, 40, 56, 255));
        break;
      case TileKind::kDiscord:
        DrawUtilityTile(a, b, "Discord", -1, focused, &app.discord_icon(), IM_COL32(88, 101, 242, 255));
        break;
      case TileKind::kSettings: DrawUtilityTile(a, b, "Settings", 0, focused); break;
      case TileKind::kExit: DrawUtilityTile(a, b, "Exit", 1, focused); break;
    }
    if (Hover(a, b)) hovered = t;
    x += w + (t >= big ? 12 : 16) * f.s;
  }
  if (hovered >= 0 && MouseMoved() && home.focus != hovered) {
    home.focus = hovered;
    sound::Play(sound::Sound::kMove);
  }
  if (hovered >= 0 && ImGui::IsMouseClicked(0) && !ModalOpen()) {
    home.focus = hovered;
    sound::Play(sound::Sound::kSelect);
    Activate(app, hovered);
  }

  // Buttons.
  if (Pressed(Btn::kA)) Activate(app, home.focus);
  if (on_game)
    HintBar({{Btn::kA, "Open"}, {Btn::kLB, ""}, {Btn::kRB, "Switch game"}});
  else
    HintBar({{Btn::kA, "Select"}, {Btn::kLB, ""}, {Btn::kRB, "Games"}});

  char foot[96];
  std::snprintf(foot, sizeof foot, "Version %s", Project().version.c_str());
  const std::string art = app.art().status();
  Text(f.regular, 13, ImVec2(f.X(48), f.B(34)), White(0.4f), art.empty() ? std::string(foot) : art);
}

}  // namespace trg::hub
