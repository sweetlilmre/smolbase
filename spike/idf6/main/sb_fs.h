// sb_fs.h — the wrapper from docs/research/littlefs-wrapper-sketch.md, made
// real so it can be compiled and exercised on hardware before any core module
// is touched.
//
// This is a FILE ACCESS helper, not an fs::FS compatibility layer. It does
// three things and deliberately nothing else:
//   1. Path ops that return bool, so the 30 `if (!LittleFS.remove(p))` sites
//      convert without 30 chances to forget POSIX's inverted sense.
//   2. An RAII file handle, because there are 11 opens against 23 manual
//      closes and the volume mounts with max_files 10.
//   3. An RAII directory iterator, for AssetUpdate's two walk functions.
//
// It does NOT translate paths. See the sketch for why (PsychicHttp takes real
// POSIX paths, so prefixing here would create two path namespaces in Web.cpp).
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace sb::fs {

// ---- Path operations: Arduino's bool-returning semantics over POSIX ----

inline bool remove(const char* p) { return ::unlink(p) == 0; }
inline bool rename(const char* a, const char* b) { return ::rename(a, b) == 0; }
inline bool rmdir(const char* p) { return ::rmdir(p) == 0; }

// Folds EEXIST into success: Web.cpp:267 relies on mkdir being "a no-op when
// it exists", which Arduino's LittleFS.mkdir gave for free and POSIX does not.
inline bool mkdir(const char* p) { return ::mkdir(p, 0777) == 0 || errno == EEXIST; }

inline bool exists(const char* p) {
  struct stat st;
  return ::stat(p, &st) == 0;
}

inline bool isDir(const char* p) {
  struct stat st;
  return ::stat(p, &st) == 0 && S_ISDIR(st.st_mode);
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
  bool flushToDisk() { return _f && ::fflush(_f) == 0; }

  // ---- ArduinoJson 7 reader/writer concept ----
  // These exact signatures are what let `deserializeJson(doc, f)` and
  // `serializeJson(doc, f)` in ConfigStore.cpp survive verbatim. They double
  // as the general-purpose bulk read/write.
  int read() { return _f ? ::fgetc(_f) : -1; }
  size_t readBytes(char* buf, size_t n) { return _f ? ::fread(buf, 1, n, _f) : 0; }
  size_t write(uint8_t c) { return _f ? ::fwrite(&c, 1, 1, _f) : 0; }
  size_t write(const uint8_t* buf, size_t n) { return _f ? ::fwrite(buf, 1, n, _f) : 0; }
};

// ---- RAII directory iterator ----
// readdir() yields a basename only, so this joins it against the directory
// path — the 3 `.path()` sites in AssetUpdate.cpp need the full path.
// esp_littlefs DOES populate d_type (esp_littlefs.c: d_type = info.type ==
// LFS_TYPE_REG ? DT_REG : DT_DIR), so no stat fallback is needed.
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
    std::string path; // full path, e.g. "/littlefs/w.v0.3.3"
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

} // namespace sb::fs
