// The updater's file work (updater.h, namespace trg::update): file lists,
// SHA-256, protected paths, the staged install with undo and the retired file
// rules. Only std::filesystem, so the tests run it on real folders anywhere.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "trg/updater.h"
#include "utf8_path.h"

namespace trg::update {
namespace {

namespace fs = std::filesystem;
using detail::ToUtf8;
using detail::Utf8Path;

// ------------------------------------------------------------- SHA-256 ---
class Sha256 {
 public:
  void Update(const void* data, size_t size) {
    auto p = static_cast<const uint8_t*>(data);
    length_ += size;
    while (size) {
      const size_t take = std::min(size, size_t(64) - used_);
      std::memcpy(block_ + used_, p, take);
      used_ += take;
      p += take;
      size -= take;
      if (used_ == 64) {
        Block(block_);
        used_ = 0;
      }
    }
  }
  std::string Hex() {
    const uint64_t bits = length_ * 8;
    const uint8_t one = 0x80, zero = 0;
    Update(&one, 1);
    while (used_ != 56) Update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
    Update(len, 8);
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h_)
      for (int s = 28; s >= 0; s -= 4) out += kDigits[(v >> s) & 15];
    return out;
  }

 private:
  static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void Block(const uint8_t* b) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = uint32_t(b[i * 4]) << 24 | uint32_t(b[i * 4 + 1]) << 16 | uint32_t(b[i * 4 + 2]) << 8 | b[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
      h = g, g = f, f = e, e = d + t1, d = c, c = bb, bb = a, a = t1 + t2;
    }
    h_[0] += a, h_[1] += bb, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
  }

  uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t block_[64] = {};
  size_t used_ = 0;
  uint64_t length_ = 0;
};

// --------------------------------------------------------------- helpers ---
std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool IsHex64(std::string_view s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

void Note(const Log& log, const std::string& line) {
  if (log) log(line);
}

fs::path PathOf(const fs::path& root, std::string_view rel) { return root / Utf8Path(std::string(rel)); }

fs::path WithSuffix(fs::path p, const char* suffix) {
  p += suffix;
  return p;
}

// Removes `dir` and its parents up to (not including) `root` while they are empty.
void PruneEmpty(const fs::path& root, fs::path dir) {
  std::error_code ec;
  const fs::path stop = root.lexically_normal();
  while (!dir.empty() && dir.lexically_normal() != stop && dir.has_relative_path()) {
    if (!fs::is_directory(dir, ec) || !fs::is_empty(dir, ec)) return;
    if (!fs::remove(dir, ec)) return;
    dir = dir.parent_path();
  }
}

std::string FileHash(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return {};
  Sha256 h;
  std::vector<char> buf(1 << 20);
  while (in) {
    in.read(buf.data(), std::streamsize(buf.size()));
    if (in.gcount() > 0) h.Update(buf.data(), size_t(in.gcount()));
  }
  if (in.bad()) return {};
  return h.Hex();
}

}  // namespace

// ------------------------------------------------------------ manifests ---
const FileEntry* Manifest::Find(std::string_view path) const {
  const std::string key = Lower(path);
  for (const auto& f : files)
    if (Lower(f.path) == key) return &f;
  return nullptr;
}

bool SafePath(std::string_view path) {
  if (path.empty() || path.size() > 1024 || path.front() == '/') return false;
  for (char c : path)
    if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':') return false;
  for (size_t start = 0; start <= path.size();) {
    const size_t end = std::min(path.find('/', start), path.size());
    const std::string_view part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return false;
    start = end + 1;
  }
  return true;
}

std::optional<Manifest> ParseManifest(std::string_view text, std::string* error) {
  Manifest m;
  auto bad = [&](size_t line, const std::string& why) -> std::optional<Manifest> {
    if (error) *error = "line " + std::to_string(line) + ": " + why;
    return std::nullopt;
  };
  size_t line_no = 0;
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(at, end - at);
    at = end + 1;
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line_no == 1 && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);  // BOM
    if (line.empty() || line.front() == '#') continue;
    if (line.substr(0, 8) == "version ") {
      m.version = std::string(line.substr(8));
      continue;
    }
    const size_t s1 = line.find(' ');
    const size_t s2 = s1 == std::string_view::npos ? s1 : line.find(' ', s1 + 1);
    if (s2 == std::string_view::npos) return bad(line_no, "not \"<sha256> <size> <path>\"");
    FileEntry f;
    f.sha256 = Lower(line.substr(0, s1));
    const std::string size(line.substr(s1 + 1, s2 - s1 - 1));
    f.path = std::string(line.substr(s2 + 1));
    if (!IsHex64(f.sha256)) return bad(line_no, "not a sha256");
    if (size.empty() || size.size() > 15 || size.find_first_not_of("0123456789") != std::string::npos)
      return bad(line_no, "not a size");
    f.size = std::strtoull(size.c_str(), nullptr, 10);
    if (!SafePath(f.path)) return bad(line_no, "unsafe path " + f.path);
    m.files.push_back(std::move(f));
  }
  return m;
}

std::optional<Manifest> ReadManifest(const std::string& utf8_file, std::string* error) {
  std::ifstream in(Utf8Path(utf8_file), std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read";
    return std::nullopt;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return ParseManifest(text, error);
}

// ------------------------------------------------------ protected paths ---
ProtectedPaths::ProtectedPaths(const std::vector<std::string>& entries, const std::string& utf8_program_dir) {
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(Utf8Path(utf8_program_dir), ec);
  for (const std::string& e : entries) {
    fs::path p = Utf8Path(e);
    if (p.is_absolute()) {
      const fs::path rel = fs::weakly_canonical(p, ec).lexically_relative(root);
      const std::string r = ToUtf8(rel);
      if (ec || rel.empty() || r == "." || r.rfind("..", 0) == 0) continue;  // outside the program folder
      Add(ToUtf8(rel));
    } else {
      Add(e);
    }
  }
}

void ProtectedPaths::Add(std::string rel_path) {
  std::string p = Lower(rel_path);
  std::replace(p.begin(), p.end(), '\\', '/');
  while (p.rfind("./", 0) == 0) p.erase(0, 2);
  while (!p.empty() && p.back() == '/') p.pop_back();
  if (!p.empty()) entries_.push_back(p);
}

bool ProtectedPaths::operator()(std::string_view path) const {
  const std::string p = Lower(path);
  for (const auto& e : entries_)
    if (p == e || (p.size() > e.size() && p.compare(0, e.size(), e) == 0 && p[e.size()] == '/')) return true;
  return false;
}

// --------------------------------------------------------------- hashes ---
std::string SumFor(std::string_view sums, std::string_view file_name) {
  for (size_t at = 0; at < sums.size();) {
    size_t end = sums.find('\n', at);
    if (end == std::string_view::npos) end = sums.size();
    std::string_view line = sums.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.size() < 66 || !IsHex64(line.substr(0, 64)) || line[64] != ' ') continue;
    std::string_view name = line.substr(65);
    if (!name.empty() && (name.front() == ' ' || name.front() == '*')) name.remove_prefix(1);
    if (name == file_name) return Lower(line.substr(0, 64));
  }
  return {};
}

std::string Sha256Hex(const void* data, size_t size) {
  Sha256 h;
  h.Update(data, size);
  return h.Hex();
}

std::string FileSha256(const std::string& utf8_file) { return FileHash(Utf8Path(utf8_file)); }

// ---------------------------------------------------------- installing ---
std::vector<std::string> MissingOrWrong(const std::string& utf8_root, const Manifest& m, const ProtectedPaths& keep,
                                        uint64_t hash_limit) {
  const fs::path root = Utf8Path(utf8_root);
  std::vector<std::string> out;
  for (const auto& f : m.files) {
    if (keep(f.path)) continue;
    const fs::path p = PathOf(root, f.path);
    std::error_code ec;
    const bool file = fs::is_regular_file(p, ec);
    const uint64_t size = file ? fs::file_size(p, ec) : 0;
    if (!file || ec || size != f.size || (size <= hash_limit && FileHash(p) != f.sha256)) out.push_back(f.path);
  }
  return out;
}

InstallResult InstallFiles(const std::string& utf8_src_root, const std::string& utf8_dst_root,
                           const std::vector<std::string>& paths, const Log& log) {
  const fs::path src_root = Utf8Path(utf8_src_root), dst_root = Utf8Path(utf8_dst_root);
  std::error_code ec;
  std::vector<fs::path> created_dirs;  // made for this install: removed again if it is undone
  std::vector<fs::path> staged;        // <target>.new
  auto unstage = [&] {
    for (const auto& p : staged) fs::remove(p, ec);
    for (auto it = created_dirs.rbegin(); it != created_dirs.rend(); ++it) PruneEmpty(dst_root, *it);
  };
  // 1. Stage every file beside its target.
  for (const auto& rel : paths) {
    if (!SafePath(rel)) {
      unstage();
      return {Outcome::kNoSource, rel};
    }
    const fs::path src = PathOf(src_root, rel), dst = PathOf(dst_root, rel);
    if (!fs::is_regular_file(src, ec)) {
      unstage();
      Note(log, "install: not in the update: " + rel);
      return {Outcome::kNoSource, rel};
    }
    std::vector<fs::path> made;
    for (fs::path d = dst.parent_path(); !d.empty() && d != dst_root && !fs::exists(d, ec); d = d.parent_path())
      made.insert(made.begin(), d);
    created_dirs.insert(created_dirs.end(), made.begin(), made.end());
    fs::create_directories(dst.parent_path(), ec);
    const fs::path tmp = WithSuffix(dst, ".new");
    if (!fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec)) {
      Note(log, "install: could not write " + rel + ".new: " + ec.message());
      unstage();
      return {Outcome::kWrite, rel};
    }
    staged.push_back(tmp);
  }
  // 2. Swap them in, with undo.
  struct Swapped {
    fs::path dst;
    bool had_old;
  };
  std::vector<Swapped> done;
  auto undo = [&] {
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
      std::error_code e;
      fs::remove(it->dst, e);
      if (e) fs::rename(it->dst, WithSuffix(it->dst, ".undo"), e);  // never lose the old one
      if (it->had_old) {
        fs::rename(WithSuffix(it->dst, ".old"), it->dst, e);
        if (e) Note(log, "install: UNDO could not put back " + ToUtf8(it->dst) + ": " + e.message());
      }
    }
    unstage();
  };
  for (size_t i = 0; i < paths.size(); ++i) {
    const fs::path dst = PathOf(dst_root, paths[i]), tmp = staged[i], old = WithSuffix(dst, ".old");
    const bool had = fs::exists(dst, ec);
    if (had) {
      if (fs::exists(old, ec)) fs::remove(old, ec);  // left by an earlier update
      fs::rename(dst, old, ec);
      if (ec) {
        Note(log, "install: " + paths[i] + " is in use (" + ec.message() + "): undoing the update");
        undo();
        return {Outcome::kInUse, paths[i]};
      }
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
      Note(log, "install: could not put " + paths[i] + " in place (" + ec.message() + "): undoing the update");
      if (had) {
        std::error_code e;
        fs::rename(old, dst, e);
      }
      undo();
      return {Outcome::kInUse, paths[i]};
    }
    done.push_back({dst, had});
  }
  // 3. The old copies (the running program's own stay until CleanUpAfterUpdate).
  for (const auto& d : done)
    if (d.had_old) fs::remove(WithSuffix(d.dst, ".old"), ec);
  Note(log, "install: " + std::to_string(paths.size()) + " files installed");
  return {};
}

std::vector<std::string> RetireFiles(const std::string& utf8_root, const Manifest& retired, const Manifest& current,
                                     const ProtectedPaths& keep, const Log& log) {
  const fs::path root = Utf8Path(utf8_root);
  std::map<std::string, std::vector<const FileEntry*>> by_path;  // lower case path: its listed versions
  for (const auto& f : retired.files)
    if (!current.Find(f.path) && !keep(f.path)) by_path[Lower(f.path)].push_back(&f);
  std::vector<std::string> removed;
  std::set<fs::path> dirs;
  for (const auto& [key, versions] : by_path) {
    const std::string& rel = versions.front()->path;
    const fs::path p = PathOf(root, rel);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) continue;
    const uint64_t size = fs::file_size(p, ec);
    if (ec) continue;
    bool same = false;
    std::string sha;
    for (const auto* v : versions) {
      if (v->size != size) continue;
      if (sha.empty()) sha = FileHash(p);
      same |= v->sha256 == sha;
    }
    if (!same) {
      Note(log, "retire: kept " + rel + " (changed)");
      continue;
    }
    if (!fs::remove(p, ec)) {  // in use: renamed, so CleanUpAfterUpdate removes it later
      std::error_code e;
      fs::rename(p, WithSuffix(p, ".old"), e);
      if (e) {
        Note(log, "retire: could not remove " + rel + ": " + ec.message());
        continue;
      }
    }
    removed.push_back(rel);
    dirs.insert(p.parent_path());
    Note(log, "retire: removed " + rel);
  }
  for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) PruneEmpty(root, *it);
  return removed;
}

void RemoveOldCopies(const std::string& utf8_root, const Manifest& m) {
  const fs::path root = Utf8Path(utf8_root);
  std::error_code ec;
  for (const auto& f : m.files) {
    const fs::path old = WithSuffix(PathOf(root, f.path), ".old");
    if (fs::is_regular_file(old, ec)) fs::remove(old, ec);
  }
}

SourceResult InstallFromSource(const std::string& utf8_src_root, const std::string& utf8_dst_root, const Manifest& m,
                               const std::vector<std::string>& needed, const Log& log) {
  const fs::path src_root = Utf8Path(utf8_src_root);
  SourceResult r;
  std::vector<std::string> ok;
  for (const auto& rel : needed) {
    const FileEntry* f = m.Find(rel);
    const fs::path src = PathOf(src_root, rel);
    std::error_code ec;
    if (f && fs::is_regular_file(src, ec) && fs::file_size(src, ec) == f->size && FileHash(src) == f->sha256) {
      ok.push_back(f->path);
    } else {
      r.unavailable.push_back(rel);
      Note(log, "the download has no matching copy of " + rel);
    }
  }
  if (!ok.empty()) r.install = InstallFiles(utf8_src_root, utf8_dst_root, ok, log);
  if (r.install.outcome == Outcome::kOk) r.installed = ok;
  return r;
}

}  // namespace trg::update
