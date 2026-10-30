#include "install.h"

#include <cstdio>
#include <filesystem>

#include <trg/xbox360.h>

#include "android.h"
#include "package.h"

namespace trg::hub {
namespace fs = std::filesystem;

std::string g_release_server;

trg::UpdaterConfig GameUpdaterConfig(const GameInfo& game) {
  trg::UpdaterConfig c;
  if (!g_release_server.empty()) c.api_base = g_release_server;
  c.stable_repo = Project().repo;
  c.asset = std::string(game.zip_folder) + "-*-windows-x64.zip";  // its own zip in the packages release
  c.tag_prefix = Project().packages_tag_prefix;
  c.current_version = InstalledVersion(game);
  if (c.current_version.empty()) c.current_version = "0.0.0";
  c.program_dir = GameDir(game);
  c.program_name = game.exe;
  // Never written or deleted by an update: the game files from the player's copy, the settings the
  // launcher keeps for the game, logs and crash reports.
  c.protected_paths = {"game", std::string(game.id) + ".toml", "logs", "fpe.log"};
  c.work_dir = LauncherDir() + "/cache/downloads/" + game.id;
  c.state_file = GameDir(game) + "/update-state.txt";
  return c;
}

std::string DescribeTitleId(uint32_t title_id, InstallSource source) {
  const bool disc = source == InstallSource::kDisc;
  for (const GameInfo& g : Games()) {
    if (g.title_id == title_id) return disc ? std::string("the Xbox 360 disc of ") + g.title : std::string(g.title);
  }
  char buf[96];
  std::snprintf(buf, sizeof buf, disc ? "an Xbox 360 disc of another game (title ID %08X)" : "another game (title ID %08X)",
                title_id);
  return buf;
}

void StartInstall(const GameInfo& game, const std::string& path, GameJobs& jobs) {
  if (game.source == InstallSource::kApk)
    StartApkInstall(game, path, jobs);
  else if (game.source == InstallSource::kPackage)
    StartPackageInstall(game, path, jobs);
  else
    StartDiscInstall(game, path, jobs);
}

void StartDownload(const GameInfo& game, GameJobs& jobs, bool quiet) {
  if (jobs.task.running()) return;
#if !defined(_WIN32)
  // On Linux the games come inside the launcher's AppImage: they update when the launcher does.
  (void)game;
  if (!quiet) {
    jobs.message = "On Linux the games update together with the launcher.";
    jobs.message_is_error = false;
  }
  return;
#endif
  jobs.quiet = quiet;
  jobs.kind = GameJobs::Kind::kDownload;
  jobs.message.clear();
  jobs.message_is_error = false;
  jobs.check = {};
  jobs.report = {};
  jobs.from_version = InstalledVersion(game);
  const trg::UpdaterConfig config = GameUpdaterConfig(game);
  jobs.task.Start(std::string("Downloading ") + game.title, [config, &jobs](trg::Task& task) -> std::string {
    std::error_code ec;
    fs::create_directories(fs::u8path(config.program_dir), ec);
    std::string err = trg::CheckForUpdate(task, config, "stable", false, &jobs.check);
    if (!err.empty()) return err;
    if (!jobs.check.found) return std::string();  // nothing newer: check.message says so
    task.SetLabel("Downloading version " + jobs.check.found->version);
    return trg::InstallUpdate(task, config, *jobs.check.found, &jobs.report);
  });
}

void StartUpdateCheck(const GameInfo& game, GameJobs& jobs) {
  if (jobs.task.running()) return;
  jobs.kind = GameJobs::Kind::kUpdateCheck;
  jobs.check = {};
  const trg::UpdaterConfig config = GameUpdaterConfig(game);
  jobs.task.Start("Checking for updates", [config, &jobs](trg::Task& task) {
    return trg::CheckForUpdate(task, config, "stable", false, &jobs.check);
  });
}

void StartDiscInstall(const GameInfo& game, const std::string& image, GameJobs& jobs) {
  if (jobs.task.running()) return;
  jobs.kind = GameJobs::Kind::kInstall;
  jobs.message.clear();
  jobs.message_is_error = false;
  const std::string out_dir = GameDataDir(game);
  const uint32_t want = game.title_id;
  const std::string title = game.title;
  jobs.task.Start("Checking the disc image", [image, out_dir, want, title](trg::Task& task) -> std::string {
    const uint32_t got = trg::ReadXbox360TitleId(image);
    if (got == 0) {
      return "That file is not an Xbox 360 disc image. Choose the .iso file you made from your " + title + " disc.";
    }
    if (got != want) {
      return "That file is " + DescribeTitleId(got, InstallSource::kDisc) + ", not " + title + ". Choose the " + title +
             " disc image.";
    }
    const std::string staging = out_dir + ".part";
    std::error_code ec;
    fs::remove_all(fs::u8path(staging), ec);
    task.SetLabel("Copying the game's files from your disc image. This takes a few minutes.");
    std::string err = trg::ExtractXbox360Disc(task, image, staging);
    if (!err.empty()) {
      fs::remove_all(fs::u8path(staging), ec);
      return err;
    }
    if (!fs::exists(fs::u8path(staging + "/default.xex"), ec)) {
      fs::remove_all(fs::u8path(staging), ec);
      return "The disc image was read, but default.xex was not on it. The image may be damaged.";
    }
    fs::remove_all(fs::u8path(out_dir), ec);
    fs::rename(fs::u8path(staging), fs::u8path(out_dir), ec);
    if (ec) return "The files were copied, but the game folder could not be finished: " + ec.message();
    return std::string();
  });
}

// From Trials: The Recompilation Collection (Xbox Live Arcade games).
void StartPackageInstall(const GameInfo& game, const std::string& path, GameJobs& jobs) {
  if (jobs.task.running()) return;
  jobs.kind = GameJobs::Kind::kInstall;
  jobs.message.clear();
  jobs.message_is_error = false;
  const std::string out_dir = GameDataDir(game);
  const uint32_t want = game.title_id;
  const std::string title = game.title;
  jobs.task.Start("Looking at your copy of the game", [path, out_dir, want, title](trg::Task& task) -> std::string {
    const GameSource source = FindGameSource(path, want);
    if (source.title_id == 0) {
      return "No Xbox 360 game package was found there. Choose the " + title +
             " package file (in its 000D0000 folder), the folder holding it, or a folder it was extracted to.";
    }
    if (source.title_id != want)
      return "That is " + DescribeTitleId(source.title_id, InstallSource::kPackage) + ", not " + title +
             ". Choose your " + title + " package.";
    const std::string staging = out_dir + ".part";
    std::error_code ec;
    fs::remove_all(fs::u8path(staging), ec);
    task.SetLabel("Copying the game's files from your package. This takes a minute or two.");
    std::string err = InstallGameSource(task, source, staging);
    if (!err.empty()) {
      fs::remove_all(fs::u8path(staging), ec);
      return err;
    }
    if (!fs::exists(fs::u8path(staging + "/default.xex"), ec)) {
      fs::remove_all(fs::u8path(staging), ec);
      return "The package was read, but default.xex was not in it. The package may be damaged.";
    }
    fs::remove_all(fs::u8path(out_dir), ec);
    fs::rename(fs::u8path(staging), fs::u8path(out_dir), ec);
    if (ec) return "The files were copied, but the game folder could not be finished: " + ec.message();
    return std::string();
  });
}

bool PollJobs(const GameInfo& game, GameJobs& jobs) {
  std::string error;
  const trg::Task::State state = jobs.task.Finish(&error);
  if (state == trg::Task::State::kIdle) return false;
  const GameJobs::Kind kind = jobs.kind;
  jobs.kind = GameJobs::Kind::kNone;
  if (state == trg::Task::State::kCancelled) {
    jobs.message = "Cancelled.";
    jobs.message_is_error = false;
    return true;
  }
  if (state == trg::Task::State::kFailed) {
    jobs.message = error.empty() ? "Something went wrong." : error;
    jobs.message_is_error = true;
    return true;
  }
  jobs.message_is_error = false;
  switch (kind) {
    case GameJobs::Kind::kDownload:
      if (jobs.report.installed) {
        jobs.message = jobs.from_version.empty()
                           ? std::string(game.title) + " version " + InstalledVersion(game) + " is downloaded."
                           : std::string(game.title) + " is updated to version " + InstalledVersion(game) + ".";
        jobs.update_found.clear();
      } else {
        jobs.message = jobs.check.message.empty() ? "No release is out yet." : jobs.check.message;
      }
      break;
    case GameJobs::Kind::kUpdateCheck:
      jobs.update_found = jobs.check.found ? jobs.check.found->version : std::string();
      break;
    case GameJobs::Kind::kInstall:
      jobs.message = std::string("Installed. Your copy of ") + game.title + " is ready to play.";
      break;
    default:
      break;
  }
  return true;
}

}  // namespace trg::hub
