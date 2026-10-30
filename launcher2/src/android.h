// Android games (InstallSource::kApk): reading the player's .apk, finding its
// OBB expansion file, finding copies on the PC for the install page, and the
// install job that hands them to the project's GameInfo::install_apk.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "games.h"

namespace trg::hub {

struct GameJobs;

// The package name in an .apk's AndroidManifest.xml ("" when the file is not an APK). Only checks whether
// `want` is the package, as the binary manifest holds it as a UTF-16 string.
bool ApkHasPackage(const std::string& apk, const std::string& want);
// The OBB for `package` beside the .apk (main.<version>.<package>.obb), or in the usual Android/obb/<package>
// folder layout next to it; "" when none is found.
std::string FindObbFor(const std::string& apk, const std::string& package);

struct FoundApk {
  std::string path;  // UTF-8
  std::string obb;   // "" when not found beside it
  long long size = 0;
};

// Looks through the usual folders (Desktop, Downloads, Documents) a few folders deep for .apk files of the
// games' packages, so the install page can offer them as buttons.
class ApkFinder {
 public:
  ~ApkFinder();
  void Start();
  void Rescan();
  bool running() const { return running_; }
  std::vector<FoundApk> Found(const std::string& package) const;

 private:
  void Run();
  std::thread worker_;
  std::atomic<bool> running_{false}, stop_{false}, done_{false};
  mutable std::mutex mu_;
  std::vector<std::pair<std::string, FoundApk>> found_;  // {package, apk}
};

// Checks the package, finds the OBB, then runs game.install_apk into GameDir.
void StartApkInstall(const GameInfo& game, const std::string& apk, GameJobs& jobs);

}  // namespace trg::hub
