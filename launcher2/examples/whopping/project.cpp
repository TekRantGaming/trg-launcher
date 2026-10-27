// The Whopping Recompilation Collection's launcher as a TRG Launcher 2.0 project: everything specific to the
// collection is in this one file. Copy it (with CMakeLists.txt, launcher.rc and your own logo) to start a new
// project, then change every value. See launcher2/README.md.

#include <trg/hub.h>

#include "logo_png.h"

namespace {

trg::hub::ProjectConfig Whopping() {
  using trg::hub::GameInfo;
  using trg::hub::InstallSource;

  trg::hub::ProjectConfig p;
  p.name = "The Whopping Recompilation Collection";
  p.id = "whopping";
  p.version = PROJECT_VERSION;  // from CMake (the project's VERSION file)
  p.home_line = "All 3 games in one collection";
  p.about = "One launcher for the three Burger King Xbox 360 games: Sneak King, Big Bumpin' and PocketBike Racer. "
            "Each game is a native PC recompilation and needs your own disc.";
  p.made_by = "Blitz Games for King Games, 2006";
  p.region = "USA (NTSC)";
  p.logo_png = kWhoppingLogoPng;
  p.logo_png_size = sizeof kWhoppingLogoPng;

  // Art: picked on each game's SteamGridDB page (https://www.steamgriddb.com/game/<id>). Open an image there and
  // copy the part of its address after https://cdn2.steamgriddb.com/. Other images on those pages, for swapping:
  //   Sneak King grids   b6b28486cb0a1675344ad86a6f7158ed.jpg, ac5e37a014b907541cc5964917631b0b.png,
  //                      c1a95432b61cbb80a437ae43e2851c68.jpg; hero 24648affebbc9bd26c25ca2204036b60.jpg
  //   Big Bumpin' grids  9614aed0f74fc5c205ab85c5819c0d88.png, f339455964a1d9e602cebb370b310783.png,
  //                      8569199a7cbd506df106d3bfda2e21e3.png; hero 5dd808ef0d37652165522bd8b73b795e.jpg
  //   PocketBike grids   9c530a4028d8196f9b88e305b978a50c.png, 628612ad1d84ed962e68713df78c81d7.jpg;
  //                      hero 6648cb0e60b6ebcb33f638831097eb8b.jpg
  // The disc facts come from the dumps the ports were made with and their default.xex.
  GameInfo sneak_king = {
      "sneak_king", "Sneak King", "sneak_king.exe", "SneakKing", 0x4D538802,
      ImVec4(0.93f, 0.62f, 0.10f, 1.0f), "Deliver the goods without being seen.", InstallSource::kDisc,
      5315547, "grid/1acc3f304807556a4aa25c38b681210b.png", "hero/77161957d9238f826b23a955f6a0b5ce.png",
      "logo/27c3025bb610524e6fc37870205dd0e7.png",
      "0.0.0.70", "Burger King Sneak King.iso", "7,027,228,672 bytes", "BDE18507EBF74060D43625BE8CE2B20C604DAF3E",
      "74C91928178C93D81C9077D74331FBFD", "D1C0F3FE", "5D787B9D", "about 1.6 GB"};
  sneak_king.own_mouse_look = true;  // its mouse_look.cpp turns the camera itself
  GameInfo big_bumpin = {
      "big_bumpin", "Big Bumpin'", "big_bumpin.exe", "BigBumpin", 0x4D538801,
      ImVec4(0.90f, 0.22f, 0.20f, 1.0f), "Bumper car battles at the fair.", InstallSource::kDisc,
      5315548, "grid/2030310c6edc8298f0c3765834d0636e.png", "hero/ccfc2d538ddff519d893a6b966a1c4f1.png",
      "logo/1c555091db0a110c5f994d8f1866571e.png",
      "0.0.0.70", "Big Bumpin' (USA).iso", "7,825,162,240 bytes", "51FBF8E78A25D466295568FBE9752C7070961779",
      "9BA7CD5F4B589A80A6C5E876F4A2B26E", "0D2B2716", "56F9F46F", "about 2.0 GB"};
  GameInfo pocketbike = {
      "pocketbike_racer", "PocketBike Racer", "pocketbike_racer.exe", "PocketBikeRacer", 0x4D538803,
      ImVec4(0.18f, 0.48f, 0.90f, 1.0f), "Tiny bikes, big races.", InstallSource::kDisc,
      5315546, "grid/a738df772803bc3aa4c117a55b197b71.jpg", "hero/bed402ffececc1f61e200943dc38b89d.png",
      "logo/59ff15c69ad138635326c3f8267af16e.png",
      "0.0.0.70", "PocketBike Racer (USA).iso", "7,825,162,240 bytes", "6A801254B1691717445253A1F4E068115F16F7B9",
      "FA5A17171280CE21AFD58B7EB5F7035A", "A2F832A7", "346E875D", "about 1.2 GB"};
  p.games = {sneak_king, big_bumpin, pocketbike};

  // Releases: vX holds WhoppingCollection-vX-windows-x64.zip for players; packages-vX holds what the launcher
  // downloads (WhoppingCollection-, SneakKing-, BigBumpin-, PocketBikeRacer-vX-windows-x64.zip, SHA256SUMS.txt).
  p.repo = "TekRantGaming/whopping-recompilation-collection";
  p.packages_tag_prefix = "packages-v";
  p.launcher_zip = "WhoppingCollection";
  p.launcher_exe = "whopping_launcher";
  p.linux_data_folder = "whopping_collection";

  // The games' own settings start with bk_ (their input, frame pacing, render quality and toast hooks).
  p.setting_prefix = "bk_";
  p.from_launcher_flag = "--bk_from_launcher=true";
  p.test_background_env = "BK_TEST_BACKGROUND";
  p.release_server_env = "WHOPPING_RELEASE_SERVER";

  p.discord_url = "https://discord.gg/8eakYSvBZS";
  p.steam_art_base = "https://raw.githubusercontent.com/TekRantGaming/whopping-recompilation-collection/main/docs/steam/";
  // p.other_projects is the TRG list (trg::hub::TrgProjects()) unless set here.
  return p;
}

}  // namespace

int main(int argc, char** argv) { return trg::hub::Run(Whopping(), argc, argv); }
