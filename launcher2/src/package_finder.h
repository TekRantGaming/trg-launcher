// Finds the player's game packages, so the install page can offer them as
// buttons: the Windows file dialog cannot be used with a controller. Looks
// through the usual folders (Desktop, Downloads, Documents) and every fixed
// drive a few folders deep for Xbox Live Arcade packages (files in a 000D0000
// folder) and extracted games (default.xex), and keeps those whose title ID is
// one of the collection's games.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trg::hub {

struct FoundPackage {
  std::string path;  // UTF-8: the package file, or the folder holding default.xex
  uint32_t title_id = 0;
  long long size = 0;
  bool extracted = false;
};

class PackageFinder {
 public:
  ~PackageFinder();
  void Start();  // once; later calls do nothing while a scan runs or after it finished
  void Rescan();
  bool running() const { return running_; }
  std::vector<FoundPackage> Found(uint32_t title_id) const;

 private:
  void Run();
  std::thread worker_;
  std::atomic<bool> running_{false}, stop_{false}, done_{false};
  mutable std::mutex mu_;
  std::vector<FoundPackage> found_;
};

}  // namespace trg::hub
