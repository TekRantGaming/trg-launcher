#include "psx.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <SDL.h>

namespace trg::hub {
namespace fs = std::filesystem;

namespace {

std::string Utf8(const fs::path& p) {
  auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

std::string Extension(const std::string& path) { return Lower(Utf8(fs::u8path(path).extension())); }

// ------------------------------------------------------------ disc image ---

// The files a .cue names (FILE "name" BINARY), in order, as they are written in it.
std::vector<std::string> CueFiles(const std::string& cue) {
  std::vector<std::string> out;
  std::ifstream in(fs::u8path(cue));
  for (std::string line; std::getline(in, line);) {
    const std::string t = Trim(line);
    if (t.size() < 5 || Lower(t.substr(0, 5)) != "file ") continue;
    std::string rest = Trim(t.substr(5));
    std::string name;
    if (!rest.empty() && rest[0] == '"') {
      const size_t end = rest.find('"', 1);
      if (end == std::string::npos) continue;
      name = rest.substr(1, end - 1);
    } else {
      name = rest.substr(0, rest.find(' '));
    }
    if (!name.empty()) out.push_back(name);
  }
  return out;
}

// Reads 2048 byte sectors of the disc's data track: raw 2352 byte sectors (Mode 1 or Mode 2 Form 1) or a plain
// 2048 byte image.
class DataTrack {
 public:
  explicit DataTrack(const std::string& path) : in_(fs::u8path(path), std::ios::binary) {
    if (!in_) return;
    unsigned char head[16] = {};
    in_.seekg(16 * 2352);
    in_.read(reinterpret_cast<char*>(head), sizeof head);
    static const unsigned char kSync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    if (in_ && std::memcmp(head, kSync, 12) == 0) {
      sector_ = 2352;
      data_ = head[15] == 2 ? 24 : 16;  // Mode 2 Form 1 has an 8 byte subheader before the data
    }
    in_.clear();
  }

  bool Read(uint32_t lba, unsigned char* out) {
    in_.clear();
    in_.seekg(std::streamoff(lba) * sector_ + data_);
    in_.read(reinterpret_cast<char*>(out), 2048);
    return bool(in_);
  }

 private:
  std::ifstream in_;
  std::streamoff sector_ = 2048, data_ = 0;
};

uint32_t Le32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// SYSTEM.CNF's text from the root folder of the ISO 9660 file system, or "".
std::string ReadSystemCnf(const std::string& track) {
  DataTrack disc(track);
  unsigned char pvd[2048];
  if (!disc.Read(16, pvd) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) return {};
  const uint32_t root = Le32(pvd + 156 + 2), root_size = Le32(pvd + 156 + 10);
  for (uint32_t s = 0; s < std::min<uint32_t>((root_size + 2047) / 2048, 16); ++s) {
    unsigned char dir[2048];
    if (!disc.Read(root + s, dir)) return {};
    for (size_t at = 0; at + 33 < sizeof dir && dir[at] != 0; at += dir[at]) {
      const unsigned char* rec = dir + at;
      std::string name(reinterpret_cast<const char*>(rec + 33), std::min<size_t>(rec[32], sizeof dir - at - 33));
      name = name.substr(0, name.find(';'));
      if (Lower(name) != "system.cnf") continue;
      unsigned char cnf[2048];
      if (!disc.Read(Le32(rec + 2), cnf)) return {};
      return std::string(reinterpret_cast<const char*>(cnf), std::min<uint32_t>(Le32(rec + 10), 2048));
    }
  }
  return {};
}

// "BOOT = cdrom:\SLUS_012.37;1" -> "SLUS-01237".
std::string SerialFromCnf(const std::string& cnf) {
  size_t at = 0;
  while (at < cnf.size()) {
    size_t end = cnf.find('\n', at);
    if (end == std::string::npos) end = cnf.size();
    const std::string line = Trim(cnf.substr(at, end - at));
    at = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos || Lower(Trim(line.substr(0, eq))) != "boot") continue;
    std::string exe = Trim(line.substr(eq + 1));
    const size_t slash = exe.find_last_of("\\:/");
    if (slash != std::string::npos) exe = exe.substr(slash + 1);
    exe = exe.substr(0, exe.find(';'));
    std::string serial;
    for (char c : exe) {
      if (c == '_') serial += '-';
      else if (c != '.') serial += char(std::toupper(static_cast<unsigned char>(c)));
    }
    return serial;
  }
  return {};
}

// The data track of a .cue (its first file), or the image itself.
std::string DataTrackOf(const std::string& path) {
  if (Extension(path) != ".cue") return path;
  const std::vector<std::string> files = CueFiles(path);
  if (files.empty()) return {};
  return Utf8(fs::u8path(path).parent_path() / fs::u8path(files[0]));
}

// ------------------------------------------------------- settings.toml ---

// Sets `key = value` in [section] of a runtime settings file (settings.toml, input.ini), keeping every other line.
class SectionEditor {
 public:
  explicit SectionEditor(std::string path) : path_(std::move(path)) {
    std::ifstream in(fs::u8path(path_));
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines_.push_back(line);
    }
  }

  bool exists() const { return !lines_.empty(); }

  void Set(const std::string& section, const std::string& key, const std::string& value) {
    const std::string header = "[" + section + "]";
    int sec = -1, last = -1;
    for (int i = 0; i < int(lines_.size()); ++i) {
      const std::string t = Trim(lines_[size_t(i)]);
      if (!t.empty() && t[0] == '[') {
        if (sec >= 0) break;
        if (t == header) sec = last = i;
        continue;
      }
      if (sec < 0) continue;
      if (!t.empty() && t[0] != '#') last = i;
      const size_t eq = t.find('=');
      if (eq != std::string::npos && Trim(t.substr(0, eq)) == key) {
        lines_[size_t(i)] = key + " = " + value;
        return;
      }
    }
    if (sec < 0) {
      if (!lines_.empty() && !Trim(lines_.back()).empty()) lines_.push_back("");
      lines_.push_back(header);
      lines_.push_back(key + " = " + value);
      return;
    }
    lines_.insert(lines_.begin() + last + 1, key + " = " + value);
  }

  bool Save() const {
    const std::string tmp = path_ + ".new";
    {
      std::ofstream out(fs::u8path(tmp), std::ios::binary | std::ios::trunc);
      if (!out) return false;
      if (lines_.empty()) out << "# Written by the " << Project().name << " launcher.\n";
      for (const std::string& l : lines_) out << l << '\n';
      if (!out) return false;
    }
    std::error_code ec;
    fs::rename(fs::u8path(tmp), fs::u8path(path_), ec);
    return !ec;
  }

 private:
  std::string path_;
  std::vector<std::string> lines_;
};

}  // namespace

std::string ReadPsxSerial(const std::string& path) {
  const std::string track = DataTrackOf(path);
  return track.empty() ? std::string() : SerialFromCnf(ReadSystemCnf(track));
}

long long PsxImageSize(const std::string& path) {
  if (Extension(path) != ".cue") return std::max(0LL, trg::FileSize(path));
  long long size = 0;
  for (const std::string& f : CueFiles(path))
    size += std::max(0LL, trg::FileSize(Utf8(fs::u8path(path).parent_path() / fs::u8path(f))));
  return size;
}

const PsxDisc* FindPsxDisc(const GameInfo& game, const std::string& serial) {
  for (const PsxDisc& d : game.psx_discs)
    if (serial == d.serial) return &d;
  return nullptr;
}

std::string DescribePsxSerial(const std::string& serial) {
  for (const GameInfo& g : Games())
    for (const PsxDisc& d : g.psx_discs)
      if (serial == d.serial) return std::string("the ") + d.region + " disc of " + g.title;
  return "a PlayStation disc of another game (" + serial + ")";
}

InstalledPsx InstalledPsxDisc(const GameInfo& game) {
  InstalledPsx out;
  std::ifstream in(fs::u8path(GameDataDir(game) + "/disc.txt"));
  std::string serial, cue;
  if (!std::getline(in, serial) || !std::getline(in, cue)) return out;
  serial = Trim(serial);
  cue = GameDataDir(game) + "/" + Trim(cue);
  std::error_code ec;
  if (!fs::exists(fs::u8path(cue), ec)) return out;
  out.disc = FindPsxDisc(game, serial);
  if (out.disc) out.cue = cue;
  return out;
}

std::string PsxBuildDir(const GameInfo& game, const PsxDisc& disc) { return GameDir(game) + "/" + disc.folder; }
std::string PsxExePath(const GameInfo& game, const PsxDisc& disc) {
  std::string exe = game.exe;
#if !defined(_WIN32)
  if (exe.size() > 4 && exe.compare(exe.size() - 4, 4, ".exe") == 0) exe.resize(exe.size() - 4);
#endif
  return PsxBuildDir(game, disc) + "/" + exe;
}

std::string InstallPsxDisc(trg::Task& task, const GameInfo& game, const std::string& path, const std::string& out_dir) {
  const std::string title = game.title;
  const std::string ext = Extension(path);
  if (ext != ".cue" && ext != ".bin" && ext != ".iso" && ext != ".img")
    return "Choose the .cue file of your " + title + " disc image (or its .bin).";
  // A .bin chosen on its own: use the .cue beside it that names it, so every track comes along.
  std::string source = path;
  if (ext != ".cue") {
    std::error_code ec;
    const fs::path bin = fs::u8path(path);
    for (const auto& e : fs::directory_iterator(bin.parent_path(), ec)) {
      if (Lower(Utf8(e.path().extension())) != ".cue") continue;
      for (const std::string& f : CueFiles(Utf8(e.path())))
        if (Lower(f) == Lower(Utf8(bin.filename()))) source = Utf8(e.path());
    }
  }
  const std::string serial = ReadPsxSerial(source);
  if (serial.empty())
    return "That is not a PlayStation disc image, or its tracks are missing. Choose the .cue file of your " + title +
           " disc image, with its .bin beside it.";
  const PsxDisc* disc = FindPsxDisc(game, serial);
  if (!disc) return "That is " + DescribePsxSerial(serial) + ", not " + title + ". Choose your " + title + " disc image.";

  // The files to copy: the .cue and every track, or the single image with a .cue made for it.
  const fs::path from_dir = fs::u8path(source).parent_path();
  std::vector<std::string> files;
  std::string cue_name;
  if (Extension(source) == ".cue") {
    cue_name = Utf8(fs::u8path(source).filename());
    files = CueFiles(source);
    for (const std::string& f : files) {
      std::error_code ec;
      if (!fs::exists(from_dir / fs::u8path(f), ec)) return "The disc image is missing its track \"" + f + "\".";
    }
  } else {
    files = {Utf8(fs::u8path(source).filename())};
    cue_name = Utf8(fs::u8path(source).stem()) + ".cue";
  }

  const std::string staging = out_dir + ".part";
  std::error_code ec;
  fs::remove_all(fs::u8path(staging), ec);
  fs::create_directories(fs::u8path(staging), ec);
  if (ec) return "The game folder could not be made: " + ec.message();
  for (size_t i = 0; i < files.size(); ++i) {
    task.SetLabel("Copying your disc image (" + std::to_string(i + 1) + " of " + std::to_string(files.size()) + ")");
    fs::create_directories(fs::u8path(staging + "/" + files[i]).parent_path(), ec);
    std::string err = trg::CopyFileJob(task, Utf8(from_dir / fs::u8path(files[i])), staging + "/" + files[i]);
    if (!err.empty() || task.cancelled()) {
      fs::remove_all(fs::u8path(staging), ec);
      return err.empty() ? "Cancelled." : err;
    }
  }
  if (Extension(source) == ".cue") {
    std::string err = trg::CopyFileJob(task, source, staging + "/" + cue_name);
    if (!err.empty()) {
      fs::remove_all(fs::u8path(staging), ec);
      return err;
    }
  } else {
    const bool raw = trg::FileSize(staging + "/" + files[0]) % 2352 == 0;
    std::ofstream out(fs::u8path(staging + "/" + cue_name), std::ios::binary | std::ios::trunc);
    out << "FILE \"" << files[0] << "\" BINARY\r\n  TRACK 01 " << (raw ? "MODE2/2352" : "MODE1/2048")
        << "\r\n    INDEX 01 00:00:00\r\n";
  }
  {
    std::ofstream out(fs::u8path(staging + "/disc.txt"), std::ios::binary | std::ios::trunc);
    out << serial << "\n" << cue_name << "\n";
    if (!out) return "The game folder could not be written.";
  }
  if (ReadPsxSerial(staging + "/" + cue_name) != serial) {
    fs::remove_all(fs::u8path(staging), ec);
    return "The disc image was copied, but the copy could not be read. The image may be damaged.";
  }
  fs::remove_all(fs::u8path(out_dir), ec);
  fs::rename(fs::u8path(staging), fs::u8path(out_dir), ec);
  if (ec) return "The files were copied, but the game folder could not be finished: " + ec.message();
  return {};
}

std::vector<std::string> ConnectedControllers() {
  std::vector<std::string> names;
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (!SDL_IsGameController(i)) continue;
    const char* n = SDL_GameControllerNameForIndex(i);
    names.push_back(n ? n : "Controller");
  }
  return names;
}

std::string PsxOptionKey(const PsxOption& o) {
  return std::string("mod.") + o.feature + (*o.option ? std::string(".") + o.option : std::string());
}

bool PsxWidescreen(const GameInfo& game, const trg::SettingsStore& st) {
  for (const PsxOption& o : game.psx_options)
    if (o.widescreen) return st.GetBool(PsxOptionKey(o), std::string(o.def) == "true");
  return false;
}

int PsxOutputHeight(const trg::SettingsStore& st) {
  const std::string r = st.Get("resolution", "screen");
  if (r != "screen") return std::clamp(std::atoi(r.c_str()), 240, 4320);
  SDL_DisplayMode dm{};
  return SDL_GetDesktopDisplayMode(std::max(0, st.GetInt("monitor", 0)), &dm) == 0 && dm.h > 0 ? dm.h : 1080;
}

int PsxRenderHeight(const trg::SettingsStore& st) {
  const int out = PsxOutputHeight(st);
  if (st.Get("upscaler", "off") == "off") return out;
  const std::string m = st.Get("upscaler_mode", "quality");
  const double ratio = m == "performance" ? 2.0 : m == "balanced" ? 1.7 : 1.5;
  return std::max(240, int(out / ratio + 0.5));
}

namespace {

// mods/state.toml in the build: the game's own rows (GameInfo::psx_options), each feature with its on/off state
// and option values. The launcher owns the file; features it does not list keep their manifest defaults.
bool WritePsxModState(const GameInfo& game, const trg::SettingsStore& st, const std::string& build_dir) {
  if (game.psx_options.empty()) return true;
  struct Feature {
    std::string package, id;
    bool enabled = true;
    std::vector<std::pair<std::string, std::string>> values;
  };
  std::vector<Feature> features;
  auto find = [&](const PsxOption& o) -> Feature& {
    for (Feature& f : features)
      if (f.package == o.package && f.id == o.feature) return f;
    features.push_back({o.package, o.feature});
    return features.back();
  };
  for (const PsxOption& o : game.psx_options) {
    Feature& f = find(o);
    if (*o.option)
      f.values.push_back({o.option, st.Get(PsxOptionKey(o), o.def)});
    else
      f.enabled = st.GetBool(PsxOptionKey(o), std::string(o.def) == "true");
  }
  std::string text = "format_version = 2\n# Written by the " + Project().name + " launcher at Play.\n";
  for (const Feature& f : features) {
    text += "\n[[feature]]\npackage_id = " + Quote(f.package) + "\nid = " + Quote(f.id) +
            "\nenabled = " + (f.enabled ? "true" : "false") + "\n";
    if (!f.values.empty()) {
      text += "\n[feature.values]\n";
      for (const auto& [k, v] : f.values) text += k + " = " + Quote(v) + "\n";
    }
  }
  std::error_code ec;
  fs::create_directories(fs::u8path(build_dir + "/mods"), ec);
  std::ofstream out(fs::u8path(build_dir + "/mods/state.toml"), std::ios::binary | std::ios::trunc);
  out << text;
  return bool(out);
}

}  // namespace

bool WritePsxRuntimeSettings(const GameInfo& game, const trg::SettingsStore& st, const std::string& build_dir) {
  SectionEditor toml(build_dir + "/settings.toml");
  const std::string mode = st.Get("window_mode", "borderless");
  toml.Set("video", "fullscreen", mode == "fullscreen" ? "2" : mode == "borderless" ? "1" : "0");
  // One Resolution: the window (when windowed) is that tall, in the game's shape, and the game is drawn at that
  // height, or lower with an upscaler bringing it up to the window.
  const int out_h = PsxOutputHeight(st);
  const int width = PsxWidescreen(game, st) ? out_h * 16 / 9 : out_h * 4 / 3;
  toml.Set("video", "window_width", std::to_string(std::clamp(width, 640, 7680)));
  toml.Set("video", "internal_resolution", std::to_string(PsxRenderHeight(st)));
  toml.Set("video", "monitor", std::to_string(std::max(0, st.GetInt("monitor", 0))));
  toml.Set("video", "vsync", Quote(st.Get("vsync", "on")));
  toml.Set("video", "stretch", st.GetBool("stretch", false) ? "true" : "false");
  // Movies fill the width only beside a 16:9 game; with the game in 4:3 they keep its shape.
  toml.Set("video", "fmv_fill", PsxWidescreen(game, st) && st.GetBool("fmv_fill", true) ? "true" : "false");
  toml.Set("video", "fps_counter", st.GetBool("fps_counter", false) ? "true" : "false");
  const std::string upscaler = st.Get("upscaler", "off");
  toml.Set("video", "upscaler", Quote(upscaler == "fsr" || upscaler == "nis" ? upscaler : "off"));
  toml.Set("video", "upscaler_sharpness", std::to_string(std::clamp(st.GetInt("upscaler_sharpness", 50), 0, 100)));
  const bool fxaa = st.Get("antialiasing", "fxaa") == "fxaa";
  toml.Set("video", "antialiasing", fxaa ? "true" : "false");
  toml.Set("video", "fxaa", fxaa ? "true" : "false");
  toml.Set("video", "texture_filtering", Quote(st.GetBool("texture_filtering", false) ? "bilinear" : "nearest"));
  const bool stable = st.GetBool("geometry_correction", true);
  toml.Set("video", "geometry_correction", stable ? "true" : "false");
  toml.Set("video", "perspective_texturing", stable ? "true" : "false");
  toml.Set("video", "sharpen", std::to_string(std::clamp(st.GetInt("sharpen", 0), 0, 100)));
  toml.Set("video", "brightness", std::to_string(std::clamp(st.GetInt("brightness", 100), 50, 150)));
  const std::string filter = st.Get("screen_filter", "off");
  toml.Set("video", "crt_filter", Quote(filter == "off" ? "raw" : filter));
  const int scanlines = std::clamp(st.GetInt("scanlines", 0), 0, 100);
  toml.Set("video", "scanlines", scanlines > 0 ? "true" : "false");
  char strength[16];
  std::snprintf(strength, sizeof strength, "%.2f", scanlines / 100.0);
  toml.Set("video", "scanline_strength", strength);
  toml.Set("audio", "volume", std::to_string(std::clamp(st.GetInt("volume", 100), 0, 100)));
  const std::string delay = st.Get("audio_latency", "low");
  toml.Set("audio", "latency_ms", delay == "safe" ? "180" : delay == "normal" ? "150" : "120");
  toml.Set("audio", "spu_hq", st.GetBool("spu_hq", false) ? "true" : "false");

  // Players: the runtime opens a controller for a "gamepad" player as soon as one is switched on, even after the
  // game has started. Player 2 on Automatic takes a second controller, or the keyboard when player 1 has one.
  const size_t pads = ConnectedControllers().size();
  const std::string c1 = st.Get("p1_input", "auto"), c2 = st.Get("p2_input", "auto");
  const std::string p1 = c1 == "keyboard" ? "keyboard" : "gamepad";
  const size_t pads_for_p2 = p1 == "gamepad" ? (pads > 0 ? pads - 1 : 0) : pads;
  const std::string p2 = c2 == "controller" ? "gamepad"
                         : c2 == "keyboard" ? "keyboard"
                         : c2 == "none"     ? "none"
                         : pads_for_p2 >= 1 ? "gamepad"
                         : p1 == "gamepad" && pads >= 1 ? "keyboard" : "none";
  toml.Set("controller", "p1_device", Quote(p1));
  toml.Set("controller", "p2_device", Quote(p2));
  toml.Set("controller", "deadzone", std::to_string(std::clamp(st.GetInt("deadzone", 10), 0, 50) * 32767 / 100));
  // The launcher starts the game with --no-launcher; this also keeps the runtime's own launcher away when the
  // game is started by itself.
  toml.Set("launcher", "skip_launcher", "true");
  bool ok = toml.Save() && WritePsxModState(game, st, build_dir);

  SectionEditor input(build_dir + "/input.ini");
  if (input.exists()) {
    input.Set("controller", "vibration", std::to_string(std::clamp(st.GetInt("vibration", 100), 0, 100)));
    ok = input.Save() && ok;
  }
  return ok;
}

}  // namespace trg::hub
