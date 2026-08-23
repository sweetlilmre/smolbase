// File access over POSIX/VFS — the file half of the IDF 6 migration (phase 5).
//
// Promoted from spike/idf6/main/sb_fs.h, where every part of it was exercised
// on real hardware (spike check 11: an ArduinoJson round-trip straight through
// the handle, plus path ops verified in both directions).
//
// This is a FILE ACCESS helper, not an fs::FS compatibility layer. It does
// four things and deliberately nothing else:
//
//  1. Path ops that return bool. Arduino returned true-on-success; POSIX
//     returns 0-on-success. Thirty call sites read `if (!LittleFS.remove(p))`,
//     and a forgotten inversion SILENTLY REVERSES the logic — in exactly the
//     paths that heal a torn asset update or preserve settings on a failed
//     write. That is worth twelve lines to make impossible.
//  2. The three compound operations every caller needs and nobody should spell
//     twice: replace() (the temp-then-swap that makes a write atomic),
//     mkdirParents(), removeDirFlat(). Each of these existed in two or three
//     places with subtly different behaviour before it lived here.
//  3. An RAII file handle. Eleven opens against twenty-three manual closes,
//     several on early-return paths, against a mount that allows 10 open
//     files. A leaked descriptor is a reachable failure, not untidiness.
//  4. An RAII directory iterator, for AssetUpdate's two walk functions.
//
// It does NOT translate paths. See docs/research/littlefs-wrapper-sketch.md:
// prefixing here would create two path namespaces inside Web.cpp, because
// PsychicHttp takes real POSIX paths. Callers pass absolute paths built from
// SMOLBASE_FS_MOUNT.
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace Fs {

// ---- Path operations: Arduino's bool-returning semantics over POSIX ----

inline bool remove(const char* p) { return ::unlink(p) == 0; }
inline bool rename(const char* a, const char* b) { return ::rename(a, b) == 0; }
inline bool rmdir(const char* p) { return ::rmdir(p) == 0; }

// Folds EEXIST into success: Web.cpp relies on mkdir being "a no-op when it
// exists", which Arduino's LittleFS.mkdir gave for free and POSIX does not.
inline bool mkdir(const char* p) { return ::mkdir(p, 0777) == 0 || errno == EEXIST; }

inline bool exists(const char* p) {
  struct stat st;
  return ::stat(p, &st) == 0;
}

inline bool isDir(const char* p) {
  struct stat st;
  return ::stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// Replace `dst` with `tmp` — the write-to-temp-then-swap half of every safe
// write in the firmware. littlefs's rename replaces an existing destination in
// ONE metadata commit, so `dst` is never absent, not even for an instant; the
// remove-and-retry is a belt for a VFS that refuses to clobber. Three call
// sites used to spell this three different ways (one with the fallback, one
// removing unconditionally first, one with neither), which is three chances to
// leave a half-written file where a whole one used to be.
inline bool replace(const char* tmp, const char* dst) {
  if (rename(tmp, dst)) return true;
  remove(dst);
  return rename(tmp, dst);
}

// mkdir -p over the ANCESTORS of `path`, not the leaf: "/w/sub/f.gz" creates
// "/w" and "/w/sub". Existing directories fold into success, as mkdir does.
inline bool mkdirParents(const char* path) {
  const std::string p(path);
  bool ok = true;
  for (size_t i = 1; (i = p.find('/', i)) != std::string::npos; ++i)
    if (!mkdir(p.substr(0, i).c_str())) ok = false;
  return ok;
}

// ---- RAII file handle ----

class File {
  FILE* _f = nullptr;

public:
  File() = default;
  File(const char* path, const char* mode) : _f(::fopen(path, mode)) {}
  ~File() { close(); }

  File(File&& o) noexcept : _f(o._f) { o._f = nullptr; }
  File& operator=(File&& o) noexcept {
    if (this != &o) {
      close();
      _f = o._f;
      o._f = nullptr;
    }
    return *this;
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  explicit operator bool() const { return _f != nullptr; }
  void close() {
    if (_f) {
      ::fclose(_f);
      _f = nullptr;
    }
  }
  FILE* raw() { return _f; } // escape hatch for anything this misses

  long size() {
    if (!_f) return -1;
    struct stat st;
    return ::fstat(::fileno(_f), &st) == 0 ? (long)st.st_size : -1;
  }

  // ---- ArduinoJson reader/writer concept ----
  // These exact signatures are what let serializeJson(doc, f) and
  // deserializeJson(doc, f) keep working verbatim. Verified on hardware.
  // They double as the general-purpose bulk read/write.
  int read() { return _f ? ::fgetc(_f) : -1; }
  size_t readBytes(char* buf, size_t n) { return _f ? ::fread(buf, 1, n, _f) : 0; }
  size_t write(uint8_t c) { return _f ? ::fwrite(&c, 1, 1, _f) : 0; }
  size_t write(const uint8_t* buf, size_t n) { return _f ? ::fwrite(buf, 1, n, _f) : 0; }

  // Byte-buffer convenience, so binary callers need no cast at every site.
  size_t readBytes(uint8_t* buf, size_t n) { return readBytes((char*)buf, n); }

  // Positioning — AssetUpdate's tar walker seeks past each entry's content.
  bool seek(long pos) { return _f && ::fseek(_f, pos, SEEK_SET) == 0; }
  long position() { return _f ? ::ftell(_f) : -1; }
};

// ---- RAII directory iterator ----
// readdir() yields a basename only, so this joins it against the directory
// path — AssetUpdate needs full paths. esp_littlefs DOES populate d_type
// (esp_littlefs.c: d_type = info.type == LFS_TYPE_REG ? DT_REG : DT_DIR), so
// isDir needs no stat fallback; spike check 2 confirmed it on hardware by
// counting directories at the volume root.
class Dir {
  DIR* _d = nullptr;
  std::string _base;

public:
  explicit Dir(const char* path) : _d(::opendir(path)), _base(path) {}
  ~Dir() { close(); }

  Dir(Dir&& o) noexcept : _d(o._d), _base(std::move(o._base)) { o._d = nullptr; }
  Dir& operator=(Dir&& o) noexcept {
    if (this != &o) {
      close();
      _d = o._d;
      _base = std::move(o._base);
      o._d = nullptr;
    }
    return *this;
  }
  Dir(const Dir&) = delete;
  Dir& operator=(const Dir&) = delete;

  explicit operator bool() const { return _d != nullptr; }
  void close() {
    if (_d) {
      ::closedir(_d);
      _d = nullptr;
    }
  }

  struct Entry {
    std::string path; // full path
    std::string name; // basename
    bool isDir = false;
  };

  // Returns false at end of directory. Skips "." and "..".
  bool next(Entry& out) {
    if (!_d) return false;
    while (struct dirent* e = ::readdir(_d)) {
      if (e->d_name[0] == '.' &&
          (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
        continue;
      out.name = e->d_name;
      out.path = _base;
      if (!out.path.empty() && out.path.back() != '/') out.path += '/';
      out.path += e->d_name;
      out.isDir = (e->d_type == DT_DIR);
      return true;
    }
    return false;
  }
};

// ---- Directory removal ----
// Flat only: /w holds files, never subdirectories. Deletes in bounded batches
// because deleting while iterating trips some FS iterators — each pass collects
// a batch, closes the handle, then unlinks — and loops until a pass finds
// nothing, so there is NO CAP on the file count. (The version this replaces
// collected at most 24 names in one pass and silently left the 25th file
// behind, after which the rmdir failed and the caller saw "restore failed" with
// no clue why.)
//
// Returns false if a pass removes nothing while entries remain — which is what
// a subdirectory looks like, since unlink refuses one. Fails loudly rather than
// spinning.
inline bool removeDirFlat(const char* path) {
  if (!isDir(path)) return false;
  for (;;) {
    std::string batch[16];
    int n = 0;
    {
      Dir d(path);
      if (!d) return false;
      Dir::Entry e;
      while (n < 16 && d.next(e)) batch[n++] = e.path;
    }
    if (n == 0) break;
    int removed = 0;
    for (int i = 0; i < n; ++i)
      if (remove(batch[i].c_str())) ++removed;
    if (removed == 0) return false;
  }
  return rmdir(path);
}

} // namespace Fs
