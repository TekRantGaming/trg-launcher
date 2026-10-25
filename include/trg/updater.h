// Updates from a port's GitHub releases (from King Kong Recompiled's updater).
//
// The game describes where its releases live and which files are its own in
// an UpdaterConfig; the updater does the rest:
//
//   Check     asks GitHub for the newest release on the player's channel
//             (stable, or an optional beta repo) and compares versions,
//             2.0.0-beta.3 < 2.0.0 included.
//   Install   downloads the release asset (progress, Cancel), checks it
//             against the release's SHA256SUMS.txt (or its size when there is
//             none), unpacks it and installs EVERY file its file list names,
//             except protected paths (game files, saves, settings). Files are
//             staged as <name>.new, swapped in with the old ones kept as
//             <name>.old, and every old file is put back if any swap fails.
//             Files earlier releases shipped are removed only while unchanged.
//             Then the program restarts.
//   First start check
//             compares the program folder with its file list and fetches
//             whatever is missing or wrong from this version's own release
//             (an older updater, or an antivirus, may have left files out).
//   Linux     an AppImage is replaced as one file.
//
// The file list (update-files.txt by default) sits in the program folder and
// is written by tools/make_update_files.py: one "<sha256> <size> <path>" line
// per file of the release (paths relative, '/' separated, spaces allowed), a
// "version <v>" line and '#' comments. The retired list (update-retired.txt)
// has the same format and names files earlier releases shipped that this one
// does not. See docs/UPDATER.md for the lessons behind every rule here: the
// first updater a game ships is the one every later update depends on.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "trg/platform.h"

namespace trg {

class Ui;

// Same contract as HttpGet (download.h): "" on success, else an error message.
using HttpFn = std::function<std::string(const std::string& url, std::string& out, Task* task)>;
// Unpacks a zip into a folder (both UTF-8): "" on success, else an error message.
using UnpackFn = std::function<std::string(const std::string& zip, const std::string& into)>;

struct UpdaterConfig {
  std::string stable_repo;      // "Owner/repo" on GitHub
  std::string beta_repo;        // optional; may equal stable_repo (its prereleases and x.y.z-beta.N tags)
  std::string current_version;  // this build: "1.4.1" or "2.0.0-beta.3"
  // The release asset for this platform. '*' matches anything; the whole name
  // must match. A name ending in .AppImage is installed as an AppImage.
  std::string asset =
#if defined(_WIN32)
      "*windows-x64.zip";
#elif defined(__linux__)
      "*linux-x86_64.AppImage";
#else
      "";  // TODO(macOS): no update path yet
#endif
  std::string program_dir;    // the folder the port runs from (UTF-8)
  std::string program_name;   // marks the program folder inside the zip ("my_game.exe"); "": the file list does
  std::string files_list = "update-files.txt";
  std::string retired_list = "update-retired.txt";
  // Paths an update never writes or deletes: relative to program_dir ("game",
  // "saves", "settings.toml"), or absolute (the user data folder). Each entry
  // covers itself and everything inside it. Letter case is ignored.
  std::vector<std::string> protected_paths;
  std::string work_dir;    // downloads and unpacking; "": <temp>/trg_update_<repo name>
  std::string state_file;  // remembers a passed first start check; "": check at every start
  std::string tag_prefix = "v";
  std::string api_base = "https://api.github.com";
  HttpFn http;      // default HttpGet
  UnpackFn unpack;  // default: tar on Windows (part of Windows 10 and 11), unzip or bsdtar elsewhere
  std::function<void(const std::string&)> log;  // optional: one line per step (called on the worker thread)
};

// ---------------------------------------------------------------- versions ---
// <0, 0, >0 like strcmp. "v1.4.1" == "1.4.1" == "1.4.1.0"; 2.0.0-beta.2 < 2.0.0-beta.10 < 2.0.0.
int CompareVersions(std::string_view a, std::string_view b);
inline bool IsNewerVersion(std::string_view a, std::string_view b) { return CompareVersions(a, b) > 0; }
bool IsPrerelease(std::string_view version);  // has a "-beta.N" (or any "-...") part

struct Release {
  std::string version;  // without the tag prefix
  std::string channel;  // "stable" or "beta"
  std::string page_url;   // the release page (what's new)
  std::string asset_url;  // this platform's download
  std::string asset_name;
  uint64_t asset_size = 0;  // as the release lists it (0: unknown)
  std::string sums_url;     // the release's SHA256SUMS.txt; "" when it has none
};

// The release a check offers, from the newest release of each channel (nullopt:
// none, or no answer), for a build of version `current`. Not switching: a newer
// release of the channel; on beta also a newer stable whose x.y.z is at least
// current's (the beta's own final release, not an older line's hotfix).
// Switching channel: that channel's newest release whenever it differs from
// current, older too, since going back to stable can mean going down a version.
std::optional<Release> PickUpdate(std::string_view channel, bool switching, std::string_view current,
                                  const std::optional<Release>& stable, const std::optional<Release>& beta);

// The newest release in a GitHub API answer: a list (/releases) or one release
// (/releases/tags/...). Drafts, releases without a matching asset and, unless
// `allow_prereleases`, prereleases and x.y.z-beta.N tags are skipped.
std::optional<Release> ParseReleases(const std::string& json, const std::string& asset_pattern,
                                     const std::string& tag_prefix, bool allow_prereleases);

bool MatchesPattern(std::string_view name, std::string_view pattern);  // '*' wildcards, whole name

// ------------------------------------------------------------------ jobs ---
// Each runs on a Task (platform.h) and returns "" on success, else a message
// for the player. `out` is written before the job ends.

struct UpdateCheck {
  std::optional<Release> found;  // set when there is something to install
  std::string message;           // "You have the newest version (1.4.1)." and the like
};
// Asks GitHub for the newest release of `channel` ("stable" or "beta"; beta
// needs beta_repo). With `switching`, an older version is offered too.
std::string CheckForUpdate(Task& task, const UpdaterConfig& config, std::string channel, bool switching,
                           UpdateCheck* out);

struct InstallReport {
  bool installed = false;  // true once files were changed: offer the restart (even if Cancel was pressed late)
  int files = 0;           // files installed
  int retired = 0;         // old files removed
  std::string message;     // for the player
};
// Downloads, checks and installs `release` into config.program_dir.
std::string InstallUpdate(Task& task, const UpdaterConfig& config, const Release& release, InstallReport* out);

// The first start check: removes unchanged retired files, compares the
// program folder with its file list (sizes; hashes of files up to 4 MB) and
// fetches what is missing or wrong from this version's release. Does nothing
// without a file list, for another version's list, or inside an AppImage. A
// passed check is remembered in config.state_file. Protected paths are never
// touched.
std::string CheckProgramFiles(Task& task, const UpdaterConfig& config, InstallReport* out);

// Removes the <name>.old files a swap left behind (the running program's own
// are in use until it exits). Call once at startup.
void CleanUpAfterUpdate(const UpdaterConfig& config);

// Starts this program again with the same command line (on Linux, the
// AppImage itself). The caller then quits. False when it could not start.
bool RestartProgram();

// ------------------------------------------------------------------- UI ---
// The Updates rows: "Version", "Update channel: Stable / Beta" (when a beta
// repo is set), "Updates: [Check for updates]", which becomes
// "[Install version X]" and then "[Restart now]", with progress, Cancel and
// the result in words underneath. Keep the state alive with the launcher.
struct UpdatesState {
  UpdateCheck check;
  InstallReport report, files_report;
  std::string message;
  bool checking = false, installing = false, restart_ready = false;
  // Last, so they stop (and join their jobs) before the results above go away.
  Task files;  // the first start check
  Task task;   // check and install
};

struct UpdatesRowOptions {
  const char* channel_key = "update_channel";  // setting holding "stable" or "beta"
  const char* check_key = "update_check";      // "Check at startup" toggle; nullptr hides it
  const char* default_channel = "stable";
  std::function<void()> on_restart;  // default: RestartProgram(), then the launcher quits
};

void UpdatesRows(Ui& ui, UpdatesState& state, const UpdaterConfig& config, const UpdatesRowOptions& options = {});

// For the game's startup: a quiet check (the rows show its result), and the
// first start check. Both do nothing while their Task is busy.
void StartUpdateCheck(UpdatesState& state, const UpdaterConfig& config, const std::string& channel,
                      bool switching = false);
void StartProgramFilesCheck(UpdatesState& state, const UpdaterConfig& config);
// An update was found and not installed yet (for Page::needs_attention).
bool UpdateWaiting(const UpdatesState& state);

// ------------------------------------------------------------ file work ---
// The pieces the jobs above are built from, for tests and tools. Only
// std::filesystem: no network.
namespace update {

struct FileEntry {
  std::string path;  // relative, '/' separated
  uint64_t size = 0;
  std::string sha256;  // 64 lowercase hex digits
};

struct Manifest {
  std::string version;  // "" when the list has no version line
  std::vector<FileEntry> files;
  const FileEntry* Find(std::string_view path) const;  // letter case ignored; nullptr when not listed
};

// nullopt (and *error) when a line is broken, a hash or size is not valid or
// a path is not safe.
std::optional<Manifest> ParseManifest(std::string_view text, std::string* error = nullptr);
std::optional<Manifest> ReadManifest(const std::string& utf8_file, std::string* error = nullptr);

// A relative path an update may name: not empty, no drive or leading '/', no
// '\\' or ':', no "." or ".." or empty part, no control characters.
bool SafePath(std::string_view path);

class ProtectedPaths {
 public:
  ProtectedPaths() = default;
  // `entries` as in UpdaterConfig::protected_paths; absolute ones outside `program_dir` are dropped.
  ProtectedPaths(const std::vector<std::string>& entries, const std::string& utf8_program_dir);
  bool operator()(std::string_view rel_path) const;
  void Add(std::string rel_path);

 private:
  std::vector<std::string> entries_;  // lower case, '/' separated, no trailing '/'
};

// The hash SHA256SUMS.txt gives for `file_name` ("<hash>  <name>" or
// "<hash> *<name>"), lowercase; "" when it has no line for it.
std::string SumFor(std::string_view sums, std::string_view file_name);
std::string Sha256Hex(const void* data, size_t size);
std::string FileSha256(const std::string& utf8_file);  // "" when it cannot be read

using Log = std::function<void(const std::string&)>;

// Listed files whose copy under `root` is missing, of another size or (files
// up to `hash_limit` bytes) of another hash. Protected paths are skipped.
std::vector<std::string> MissingOrWrong(const std::string& utf8_root, const Manifest& m, const ProtectedPaths& keep,
                                        uint64_t hash_limit);

enum class Outcome {
  kOk,
  kNoSource,  // a file is not in the source folder (nothing changed)
  kWrite,     // a file could not be written beside its target (nothing changed)
  kInUse,     // a file could not be swapped in, so every file was put back (nothing changed)
};
struct InstallResult {
  Outcome outcome = Outcome::kOk;
  std::string path;  // the file that failed
};

// Copies `paths` from src_root over dst_root: every file staged as <name>.new
// first, then swapped in (the old one becomes <name>.old, which works for a
// running program), with every old file put back if a swap fails. The .old
// files are removed where possible; CleanUpAfterUpdate removes the rest.
InstallResult InstallFiles(const std::string& utf8_src_root, const std::string& utf8_dst_root,
                           const std::vector<std::string>& paths, const Log& log = {});

// Deletes the files `retired` lists that `current` does not, only while
// unchanged (size and sha256 equal one listed for that path), then each of
// their folders left empty. Nothing else is ever touched. Returns the paths.
std::vector<std::string> RetireFiles(const std::string& utf8_root, const Manifest& retired, const Manifest& current,
                                     const ProtectedPaths& keep, const Log& log = {});

// Removes <path>.old beside every listed file.
void RemoveOldCopies(const std::string& utf8_root, const Manifest& m);

// Of `needed`, installs the files whose copy in src_root matches `m` (size and
// sha256); the rest are `unavailable`.
struct SourceResult {
  std::vector<std::string> installed, unavailable;
  InstallResult install;
};
SourceResult InstallFromSource(const std::string& utf8_src_root, const std::string& utf8_dst_root, const Manifest& m,
                               const std::vector<std::string>& needed, const Log& log = {});

}  // namespace update
}  // namespace trg
