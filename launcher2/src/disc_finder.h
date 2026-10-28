// Finds the player's disc images, so the install page can offer them as
// buttons: the Windows file dialog cannot be used with a controller. Looks
// through the usual folders (Desktop, Downloads, Documents, Videos) and every
// fixed drive a few folders deep for .iso files, and keeps those whose title
// ID is one of the collection's games; for PlayStation games (psx.h), .cue
// files whose serial is one of their discs.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <trg/hub.h>

namespace trg::hub {

struct FoundDisc {
  std::string path;  // UTF-8
  uint32_t title_id = 0;
  std::string serial;  // a PlayStation disc ("SLUS-01237"); title_id is 0
  long long size = 0;  // a .cue: the size of its tracks
};

class DiscFinder {
 public:
  ~DiscFinder();
  void Start();  // once; later calls do nothing while a scan runs or after it finished
  void Rescan();
  bool running() const { return running_; }
  std::vector<FoundDisc> Found(const GameInfo& game) const;

 private:
  void Run();
  std::thread worker_;
  std::atomic<bool> running_{false}, stop_{false}, done_{false};
  mutable std::mutex mu_;
  std::vector<FoundDisc> found_;
};

}  // namespace trg::hub
