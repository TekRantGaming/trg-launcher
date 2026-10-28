// The install screen: the game's files come from the player's own copy, a disc
// image or an Xbox Live Arcade package (or a folder it was extracted to), as
// the game's InstallSource says. Copies found on this PC are offered as buttons
// (the Windows file dialog cannot be used with a controller); the supported
// copy is described on the right. Wording after King Kong Recompiled's "Your
// copy of the game" page.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <SDL_syswm.h>
#if defined(_WIN32)
#include <windows.h>
#include <shobjidl.h>
#endif

#include <trg/platform.h>

#include "app.h"
#include "psx.h"

namespace trg::hub {
namespace {

using namespace dash;

// The Windows file picker for .iso files. It runs on a thread of its own, so the launcher keeps drawing while it is
// open (the picker on the main thread froze the launcher, and when the picker opened behind another window there was
// nothing to see or click). It belongs to the launcher window, and is brought to the front as soon as it appears and
// again whenever the player asks.
struct Picker {
  std::mutex mu;
  bool open = false, done = false;
  int game = -1;
  std::string path;  // the chosen file; empty when cancelled
#if defined(_WIN32)
  HWND dialog = nullptr;  // the picker's window, once it is showing
#endif
};
Picker g_picker;

#if defined(_WIN32)
// The project's test_background_env (test runs) leaves it where it opens, as the games' windows do.
void BringToFront(HWND w) {
  static const bool test_background =
      !Project().test_background_env.empty() && std::getenv(Project().test_background_env.c_str()) != nullptr;
  if (!w || test_background) return;
  SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetForegroundWindow(w);
}

// Learns the picker's window when it first shows a folder, and brings it to the front.
class PickerEvents : public IFileDialogEvents {
 public:
  IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == __uuidof(IFileDialogEvents)) {
      *out = static_cast<IFileDialogEvents*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  IFACEMETHODIMP_(ULONG) Release() override {
    const ULONG n = --refs_;
    if (!n) delete this;
    return n;
  }
  IFACEMETHODIMP OnFolderChange(IFileDialog* dlg) override {
    IOleWindow* ole = nullptr;
    if (SUCCEEDED(dlg->QueryInterface(IID_PPV_ARGS(&ole)))) {
      HWND w = nullptr;
      if (SUCCEEDED(ole->GetWindow(&w)) && w) {
        bool first = false;
        {
          std::lock_guard<std::mutex> lock(g_picker.mu);
          first = !g_picker.dialog;
          g_picker.dialog = w;
        }
        if (first) BringToFront(w);
      }
      ole->Release();
    }
    return S_OK;
  }
  IFACEMETHODIMP OnFileOk(IFileDialog*) override { return S_OK; }
  IFACEMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
  IFACEMETHODIMP OnSelectionChange(IFileDialog*) override { return S_OK; }
  IFACEMETHODIMP OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override { return S_OK; }
  IFACEMETHODIMP OnTypeChange(IFileDialog*) override { return S_OK; }
  IFACEMETHODIMP OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override { return S_OK; }

 private:
  std::atomic<ULONG> refs_{1};
};

std::string ShowPicker(HWND owner, const std::wstring& title, InstallSource source, bool folder) {
  const bool package = source == InstallSource::kPackage;
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::string out;
  IFileOpenDialog* dlg = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    const COMDLG_FILTERSPEC discs[] = {{L"Xbox 360 disc images (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
    // Packages have no file name extension, so every file is shown.
    const COMDLG_FILTERSPEC packages[] = {{L"Xbox 360 game packages (all files)", L"*.*"}};
    const COMDLG_FILTERSPEC psx[] = {{L"PlayStation disc images (*.cue, *.bin, *.iso)", L"*.cue;*.bin;*.iso"},
                                     {L"All files", L"*.*"}};
    if (!folder) {
      if (package)
        dlg->SetFileTypes(1, packages);
      else if (source == InstallSource::kPsxDisc)
        dlg->SetFileTypes(2, psx);
      else
        dlg->SetFileTypes(2, discs);
    }
    dlg->SetTitle(title.c_str());
    FILEOPENDIALOGOPTIONS opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts)))
      dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                      (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    PickerEvents* events = new PickerEvents();
    DWORD cookie = 0;
    const bool advised = SUCCEEDED(dlg->Advise(events, &cookie));
    if (SUCCEEDED(dlg->Show(owner))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          const int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
          out.resize(size_t(std::max(len, 1)));
          WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), len, nullptr, nullptr);
          out.resize(std::strlen(out.c_str()));
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    if (advised) dlg->Unadvise(cookie);
    events->Release();
    dlg->Release();
  }
  if (SUCCEEDED(co)) CoUninitialize();
  return out;
}
#endif

void OpenPicker(SDL_Window* owner, int game, const std::string& title, bool folder = false) {
  const InstallSource source = Games()[game].source;
  {
    std::lock_guard<std::mutex> lock(g_picker.mu);
    if (g_picker.open) return;
    g_picker.open = true;
    g_picker.done = false;
    g_picker.game = game;
    g_picker.path.clear();
#if defined(_WIN32)
    g_picker.dialog = nullptr;
#endif
  }
#if defined(_WIN32)
  SDL_SysWMinfo wm;
  SDL_VERSION(&wm.version);
  HWND hwnd = SDL_GetWindowWMInfo(owner, &wm) ? wm.info.win.window : nullptr;
  const int n = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
  std::wstring wtitle(size_t(std::max(n, 1)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wtitle.data(), n);
  wtitle.resize(wcslen(wtitle.c_str()));
  std::thread([hwnd, wtitle, source, folder] {
    std::string path = ShowPicker(hwnd, wtitle, source, folder);
    std::lock_guard<std::mutex> lock(g_picker.mu);
    g_picker.path = std::move(path);
    g_picker.open = false;
    g_picker.done = true;
    g_picker.dialog = nullptr;
  }).detach();
#else
  (void)owner;
  (void)folder;
  std::string path = source == InstallSource::kPackage ? trg::BrowseForFile(title, {{"Xbox 360 game packages", "*"}})
                     : source == InstallSource::kPsxDisc
                         ? trg::BrowseForFile(title, {{"PlayStation disc images", "*.cue *.bin *.iso"}})
                         : trg::BrowseForFile(title, {{"Xbox 360 disc images", "*.iso"}});
  std::lock_guard<std::mutex> lock(g_picker.mu);
  g_picker.path = std::move(path);
  g_picker.open = false;
  g_picker.done = true;
#endif
}

bool PickerOpen() {
  std::lock_guard<std::mutex> lock(g_picker.mu);
  return g_picker.open;
}

void PickerToFront() {
#if defined(_WIN32)
  HWND w = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_picker.mu);
    w = g_picker.dialog;
  }
  BringToFront(w);
#endif
}

// A PlayStation port's discs: each one's region, serial and the facts of the image the port was made with.
void PsxCopyCard(const GameInfo& game, ImVec2 a, float w) {
  const Frame& f = F();
  const ProjectConfig& p = Project();
  std::vector<std::pair<std::string, std::string>> lines = {{"Game", game.title}};
  if (!p.made_by.empty()) lines.push_back({"Made by", p.made_by});
  if (!p.console.empty()) lines.push_back({"Console", p.console});
  for (const PsxDisc& d : game.psx_discs) {
    lines.push_back({"", ""});
    lines.push_back({std::string(d.region) + " disc", d.serial});
    lines.push_back({"Tested image", d.copy_name});
    lines.push_back({"Track size", d.copy_size});
    lines.push_back({"SHA1", d.copy_sha1});
    lines.push_back({"CRC32", d.copy_crc32});
  }
  const float line_h = 25 * f.s;
  float h = 58 * f.s + 64 * f.s;
  for (const auto& l : lines) h += l.first.empty() ? line_h * 0.4f : line_h;
  const ImVec2 b(a.x + w, a.y + h);
  Card(a, b, 0.08f);
  Text(f.semibold, 17, ImVec2(a.x + 20 * f.s, a.y + 18 * f.s), White(),
       game.psx_discs.size() == 1 ? "Supported disc" : "Supported discs");
  float y = a.y + 52 * f.s;
  for (const auto& [label, value] : lines) {
    if (label.empty()) {
      y += line_h * 0.4f;
      continue;
    }
    Text(f.regular, 13, ImVec2(a.x + 20 * f.s, y), White(0.55f), label);
    float size = 13;
    while (size > 9 && TextSize(f.regular, size, value).x > (w - 150) * f.s) size -= 0.5f;
    TextRight(f.regular, size, ImVec2(b.x - 20 * f.s, y + (13 - size) * 0.5f * f.s), White(0.92f), value);
    y += line_h;
  }
  Text(f.regular, 12, ImVec2(a.x + 20 * f.s, y + 8 * f.s), White(0.5f),
       "Make the image (.cue and .bin) from your own disc. Another good dump of a supported disc works too, as only "
       "its serial is checked.",
       w / f.s - 40);
}

void CopyCard(const GameInfo& game, ImVec2 a, float w) {
  if (IsPsx(game)) return PsxCopyCard(game, a, w);
  const Frame& f = F();
  const ProjectConfig& p = Project();
  const bool disc = game.source == InstallSource::kDisc;
  char id[16];
  std::snprintf(id, sizeof id, "%08X", game.title_id);
  // Everything known about the copy the port was made with. Only the title ID is checked, so another good copy of
  // the same game works too.
  std::vector<std::pair<const char*, std::string>> lines = {{"Game", game.title}};
  if (!p.made_by.empty()) lines.push_back({"Made by", p.made_by});
  if (!p.console.empty()) lines.push_back({"Console", disc ? p.console : p.console + " (Xbox Live Arcade)"});
  if (!p.region.empty()) lines.push_back({"Region", p.region});
  lines.insert(lines.end(), {
      {"Title ID", id},
      {"Media ID", game.media_id},
      {disc ? "Disc version" : "Game version", game.copy_version},
      {disc ? "Tested image" : "Tested package", game.copy_name},
      {disc ? "Image size" : "Package size", game.copy_size},
      {"SHA1", game.copy_sha1},
      {"MD5", game.copy_md5},
      {"CRC32", game.copy_crc32},
      {"Installed size", game.installed_size},
  });
  const float line_h = 25 * f.s;
  const ImVec2 b(a.x + w, a.y + 58 * f.s + line_h * float(lines.size()) + (disc ? 64 : 80) * f.s);
  Card(a, b, 0.08f);
  Text(f.semibold, 17, ImVec2(a.x + 20 * f.s, a.y + 18 * f.s), White(), disc ? "Supported disc" : "Supported package");
  float y = a.y + 52 * f.s;
  for (const auto& [label, value] : lines) {
    Text(f.regular, 13, ImVec2(a.x + 20 * f.s, y), White(0.55f), label);
    float size = 13;  // long hashes shrink to fit beside the label
    while (size > 9 && TextSize(f.regular, size, value).x > (w - 150) * f.s) size -= 0.5f;
    TextRight(f.regular, size, ImVec2(b.x - 20 * f.s, y + (13 - size) * 0.5f * f.s), White(0.92f), value);
    y += line_h;
  }
  Text(f.regular, 12, ImVec2(a.x + 20 * f.s, y + 8 * f.s), White(0.5f),
       disc ? std::string("Make the image (.iso) from your own disc. Other versions and regions will not work; another "
                          "good dump of this disc does, as only the title ID is checked.")
            : "Copy the package from your own console or its storage: it is the one file in Content\\0000000000000000\\" +
                  std::string(id) + "\\000D0000. A folder it was extracted to works too. The full game is always unlocked.",
       w / f.s - 40);
}

// The rows for an Xbox Live Arcade game: packages found on this PC, then the package file or a folder.
void PackageRows(App& app, int g, Rows& r) {
  const GameInfo& game = Games()[g];
  GameUi& u = app.game_ui(g);
  r.Paragraph("This recompilation plays the original Xbox 360 game, so it needs your own copy of " +
              std::string(game.title) + ": its Xbox Live Arcade package, copied from your console. The game's "
              "files are copied from it. Nothing from the game comes with the launcher.");
  if (u.jobs.message_is_error && !u.jobs.message.empty()) r.Paragraph(u.jobs.message, false);
  app.packages().Start();
  for (const FoundPackage& d : app.packages().Found(game.title_id)) {
    char size[32];
    std::snprintf(size, sizeof size, "%.2f GB", double(d.size) / (1024.0 * 1024.0 * 1024.0));
    const std::string name = d.extracted ? std::string("Extracted ") + game.title : std::string(game.title) + " package";
    if (r.Button(name, "Found on this PC: " + d.path + (d.extracted ? "" : "  (" + std::string(size) + ")"),
                 "Install")) {
      u.disc_path = d.path;
      StartInstall(game, d.path, u.jobs);
    }
  }
  if (app.packages().running())
    r.Paragraph(app.packages().Found(game.title_id).empty() ? "Looking for your copy on this PC..."
                                                            : "Still looking for others...");
  else if (app.packages().Found(game.title_id).empty())
    r.Paragraph("Your copy of this game was not found on this PC. Choose it yourself below.");
  if (PickerOpen()) {
    r.Paragraph("Choose your copy of the game in the file window that opened.");
    if (r.Button("Show the file window", "Brings the file window to the front, in case it opened behind."))
      PickerToFront();
  } else {
    if (r.Button("Choose package file", "The file in the 000D0000 folder. You can also drop it on this window."))
      OpenPicker(app.window(), g, "Choose your " + std::string(game.title) + " package (in its 000D0000 folder)");
#if defined(_WIN32)
    if (r.Button("Choose game folder", "A folder holding the package, or the game extracted (with default.xex)."))
      OpenPicker(app.window(), g, "Choose the folder with your " + std::string(game.title), true);
#endif
  }
  if (!app.packages().running() && r.Button("Search again", "Looks through this PC for your copy again."))
    app.packages().Rescan();
}

}  // namespace

void DrawInstall(App& app, int g) {
  const Frame& f = F();
  const GameInfo& game = Games()[g];
  GameUi& u = app.game_ui(g);

  // The file picker's answer (it may come after the player has left this screen).
  {
    std::unique_lock<std::mutex> lock(g_picker.mu);
    if (g_picker.done) {
      g_picker.done = false;
      const int pg = g_picker.game;
      const std::string file = g_picker.path;
      lock.unlock();
      if (!file.empty() && pg >= 0 && pg < GameCount()) {
        app.game_ui(pg).disc_path = file;
        StartInstall(Games()[pg], file, app.game_ui(pg).jobs);
      }
    }
  }
  // Only the install's own result: a download or the quiet startup update check finishes in App::Idle as usual.
  if (u.jobs.kind == GameJobs::Kind::kInstall && PollJobs(game, u.jobs)) {
    app.RefreshStates();
    if (!u.jobs.message_is_error && u.jobs.message != "Cancelled.") {
      app.SetToast(u.jobs.message);
      app.OpenHub(g);  // the game now has Play
      return;
    }
    if (u.jobs.message_is_error) app.SetToast(u.jobs.message, true);
  }
  if (Pressed(Btn::kB)) {
    app.OpenHub(g);
    return;
  }

  f.dl->AddRectFilled(f.o, f.end, IM_COL32(8, 8, 10, 255));
  Backdrop(app.art().Get(g, ArtKind::kHero), 0.55f, 0.72f);
  Shade();
  f.dl->AddRectFilled(f.o, f.end, Black(0.35f));
  TopBar({game.title, "Install"});
  Text(f.semibold, 28, f.P(72, 90), White(), "Your copy of the game");

  const bool busy = u.jobs.task.running();
  {
    Rows r(u.install_rows, f.P(48, 146), ImVec2(f.X(720), f.B(70)), true);
    if (busy) {
      r.Progress(u.jobs.task.label(), u.jobs.task.fraction());
      r.Paragraph("You can leave this screen: the install carries on and the game is ready when it finishes.");
      if (r.Button("Cancel", "Stops the install. Nothing is left half done.")) u.jobs.task.Cancel();
    } else if (game.source == InstallSource::kPackage) {
      PackageRows(app, g, r);
    } else {
      if (IsPsx(game)) {
        std::string regions;
        for (size_t i = 0; i < game.psx_discs.size(); ++i)
          regions += std::string(i == 0 ? "" : i + 1 == game.psx_discs.size() ? " or " : ", ") + game.psx_discs[i].region;
        r.Paragraph("This recompilation plays the original PlayStation game, so it needs a disc image (.cue and .bin) "
                    "that you made from your own " + std::string(game.title) + " disc (" + regions + "). It is copied "
                    "into the game's folder. Nothing from the game comes with the launcher.");
      } else {
        r.Paragraph("This recompilation plays the original Xbox 360 game, so it needs a disc image (.iso) that you made "
                    "from your own " + std::string(game.title) + " disc. The game's files are copied from it. Nothing "
                    "from the game comes with the launcher.");
      }
      if (u.jobs.message_is_error && !u.jobs.message.empty()) r.Paragraph(u.jobs.message, false);
      app.discs().Start();
      for (const FoundDisc& d : app.discs().Found(game)) {
        char size[32];
        if (d.size < 1024LL * 1024 * 1024)
          std::snprintf(size, sizeof size, "%.0f MB", double(d.size) / (1024.0 * 1024.0));
        else
          std::snprintf(size, sizeof size, "%.1f GB", double(d.size) / (1024.0 * 1024.0 * 1024.0));
        std::string name = std::filesystem::u8path(d.path).filename().u8string();
        if (const PsxDisc* pd = d.serial.empty() ? nullptr : FindPsxDisc(game, d.serial))
          name += std::string("  (") + pd->region + ")";
        if (r.Button(name, "Found on this PC: " + d.path + "  (" + size + ")", "Install")) {
          u.disc_path = d.path;
          StartInstall(game, d.path, u.jobs);
        }
      }
      if (app.discs().running())
        r.Paragraph(app.discs().Found(game).empty() ? "Looking for your disc image on this PC..."
                                                    : "Still looking for others...");
      else if (app.discs().Found(game).empty())
        r.Paragraph("No disc image of this game was found on this PC. Choose it yourself below.");
      if (PickerOpen()) {
        r.Paragraph("Choose your disc image in the file window that opened.");
        if (r.Button("Show the file window", "Brings the file window to the front, in case it opened behind."))
          PickerToFront();
      } else if (r.Button("Choose disc image", IsPsx(game) ? "Pick the .cue file (with its .bin beside it), or drop "
                          "it on this window." : "Pick the .iso file, or drop it on this window.")) {
        OpenPicker(app.window(), g, "Choose your " + std::string(game.title) + " disc image");
      }
      if (!app.discs().running() && r.Button("Search again", "Looks through this PC for disc images again."))
        app.discs().Rescan();
    }
  }
  CopyCard(game, f.P(752, 96), std::max(320.0f, f.size.x / f.s - 752 - 48));
  HintBar({{Btn::kA, "Select"}, {Btn::kB, "Back"}});
}

}  // namespace trg::hub
