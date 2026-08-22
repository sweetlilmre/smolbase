# LittleFS wrapper sketch (IDF 6 migration, phase 5)

**Date:** 2026-08-22
**Status:** Implemented as `spike/idf6/main/sb_fs.h` and **compiling clean on ESP-IDF 6.0.2**. Runtime behaviour is exercised by spike checks 11 and 12, which have not run on hardware yet.

## Verified since first draft

**`d_type` is populated — no `stat` fallback needed.** `managed_components/joltwallet__littlefs/src/esp_littlefs.c:2563`: `entry->d_type = info.type == LFS_TYPE_REG ? DT_REG : DT_DIR`. So `isDirectory()` is a plain `d_type == DT_DIR`. Spike check 2 now walks both `/w` and `/` through `sb::fs::Dir` and counts directories at root, so a regression to `DT_UNKNOWN` fails the check loudly instead of silently reporting zero directories.

**ArduinoJson's reader/writer concept accepts the handle.** `serializeJson(doc, f)` and `deserializeJson(doc, f)` compile against `sb::fs::File` with `int read()`, `size_t readBytes(char*, size_t)`, `size_t write(uint8_t)`, `size_t write(const uint8_t*, size_t)`. That was the one part of the sketch I flagged as needing verification, and it holds — so the two `ConfigStore.cpp` streaming sites survive verbatim. Check 11 makes it a runtime result too, not just a compile-time one.

**A root mount is legal at the VFS layer — which would dissolve the path problem entirely.** `components/vfs/vfs.c:413` reads `if (base_path_len != 0 && !is_path_prefix_valid(base_path, base_path_len))`, so a zero-length `base_path` skips prefix validation. If `esp_vfs_littlefs_register` with `base_path = ""` also works in practice, then **every existing path constant stays byte-identical** — no prefixing wrapper, no six-constant edit, no second path namespace to collide with PsychicHttp. That is strictly better than what this document originally recommended. Spike check 12 tests it, deliberately last, because registering a filesystem as the catch-all prefix could plausibly shadow the console's `/dev/uart` paths.

The two open risks on that: whether joltwallet's own code rejects an empty `base_path`, and whether longest-prefix matching really does keep `/dev/uart0` ahead of the root mount. Both are check-12 outcomes, not arguments.

## Spike footprint baseline (link time, IDF 6.0.2)

| | Bytes |
|---|---:|
| `.text` (IRAM+flash) | 87 007 |
| `.data` | 17 976 |
| `.bss` | 52 448 |
| **DRAM used** | **70 424** |
| **DRAM remaining** | **110 312** of 180 736 |
| Image size | 1 057 163 (53% of the 0x220000 slot free) |

Read that carefully before quoting it: 30 720 of the `.bss` figure is the spike's own `scratchData[240*64*2]` band buffer, and "DRAM remaining at link time" is **not** the same measurement as the Arduino build's runtime 96 KB free heap. It is a promising baseline, not a comparison. The comparable number is check 10's runtime reading, which needs the device.

## What is actually there

Filesystem-level calls across `ConfigStore.cpp`, `AssetUpdate.cpp`, `Web.cpp`, `Ota.cpp`:

| Operation | Sites | POSIX equivalent | Semantics gap |
|---|---:|---|---|
| `LittleFS.remove` | 19 | `unlink` | **bool → 0/-1** |
| `LittleFS.rename` | 7 | `rename` | **bool → 0/-1** |
| `LittleFS.mkdir` | 3 | `mkdir(path, 0777)` | **bool → 0/-1** |
| `LittleFS.rmdir` | 1 | `rmdir` | **bool → 0/-1** |
| `LittleFS.open` | 11 | `fopen` | File object → `FILE*` |
| `LittleFS.begin` / `.end` | 1 / 1 | `esp_vfs_littlefs_register` / `_unregister` | direct |
| `openNextFile` | 6 | `opendir`/`readdir`/`closedir` | different shape |
| `.close()` | 23 | `fclose` / `closedir` | manual either way |
| `.path()` | 3 | `readdir` gives basename only | must join by hand |

Two observations drive the whole design. First, **30 of the 41 filesystem-level calls are stateless path operations**, not file handles. Second, **23 manual `.close()` calls**, several of them on early-return paths like `if (dir) dir.close(); return false;`.

## The three parts, judged separately

### 1. Path-op wrappers — yes, and they are not RAII at all

The value here has nothing to do with resource management. Arduino returns `bool` (true = success); POSIX returns `0` on success and `-1` on failure. Every one of those 30 sites reads `if (!LittleFS.rename(a, b))` or `if (!LittleFS.remove(p))`. Converting them raw means 30 opportunities to forget the inversion, and a forgotten inversion **silently reverses the logic** — in code paths that are precisely the ones that heal a torn asset update or preserve a settings file on a failed write. That is the worst possible place for a silent sign flip.

Twelve lines buys immunity:

```cpp
// sb_fs.h — thin POSIX wrappers preserving Arduino's bool-returning semantics.
namespace sb::fs {
inline bool remove(const char* p) { return ::unlink(p) == 0; }
inline bool rename(const char* a, const char* b) { return ::rename(a, b) == 0; }
inline bool mkdir(const char* p) { return ::mkdir(p, 0777) == 0 || errno == EEXIST; }
inline bool rmdir(const char* p) { return ::rmdir(p) == 0; }
inline bool exists(const char* p) { struct stat st; return ::stat(p, &st) == 0; }
} // namespace sb::fs
```

Note `mkdir` folding `EEXIST` into success — `Web.cpp:267` relies on mkdir being "a no-op when it exists", which Arduino gave for free and POSIX does not.

### 2. RAII file handle — yes, this is the real one

11 opens against 23 closes, with error paths that close early and return. The device mounts with `max_files 10`, so a leaked descriptor is not a theoretical tidiness issue — it is a reachable failure that would present as asset updates mysteriously failing after N attempts. RAII removes the entire class.

```cpp
// A file that closes itself. Move-only; no copy, no double-close.
class File {
  FILE* _f = nullptr;
public:
  File() = default;
  File(const char* path, const char* mode) : _f(::fopen(path, mode)) {}
  ~File() { close(); }
  File(File&& o) noexcept : _f(o._f) { o._f = nullptr; }
  File& operator=(File&& o) noexcept {
    if (this != &o) { close(); _f = o._f; o._f = nullptr; }
    return *this;
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  explicit operator bool() const { return _f != nullptr; }
  void close() { if (_f) { ::fclose(_f); _f = nullptr; } }
  FILE* raw() { return _f; }              // escape hatch

  size_t read(void* buf, size_t n)  { return _f ? ::fread(buf, 1, n, _f) : 0; }
  size_t write(const void* b, size_t n) { return _f ? ::fwrite(b, 1, n, _f) : 0; }
  long size();                            // fstat, or seek/tell

  // ArduinoJson 7 custom reader/writer concept — keeps the two streaming
  // call sites in ConfigStore.cpp working verbatim. VERIFY: ArduinoJson's
  // concept detection needs exactly these signatures.
  int read() { return _f ? ::fgetc(_f) : -1; }
  size_t readBytes(char* b, size_t n) { return read(b, n); }
  size_t write(uint8_t c) { return write(&c, 1); }
};
```

`serializeJson(doc, f)` / `deserializeJson(doc, f)` are only 2 sites, so the ArduinoJson adapter is a bonus rather than a justification — without it you buffer `settings.json` (a few hundred bytes) and lose nothing. But if the handle satisfies the concept anyway, take it.

### 3. Directory iteration — keep it local, do not promote it

All 6 `openNextFile` sites live in two functions in `AssetUpdate.cpp` (`removeDirRecursive`, `findBackupDir`). `opendir`/`readdir`/`closedir` is a genuinely different shape and wants its own RAII type — but a `Dir` in `AssetUpdate.cpp`'s anonymous namespace is the right scope. Promoting a directory abstraction to a core header for one file's two functions is the mistake I warned about with the object APIs: an abstraction with a single caller.

Two gotchas for whoever writes it: `readdir` yields **basename only** in `d_name`, so the 3 `.path()` sites need a manual join against the directory path; and `f.isDirectory()` becomes `d_type == DT_DIR`, which depends on the filesystem populating `d_type` — **verify that `esp_littlefs` does, and fall back to `stat` if not.**

## The part I would now argue against

My earlier note floated the wrapper doing **path prefixing** — Arduino's LittleFS paths are volume-relative (`/w`, `/config/settings.json`) while POSIX needs the mount point (`/littlefs/w`), so a wrapper could prefix in one place and leave all 41 sites plus the runtime-built paths (`/w.v0.3.3`, `writePath`, `fsPath`) untouched. Superficially that is the wrapper's biggest win.

**It is a trap, and the reason is PsychicHttp.** Its static-file handler and its internal `psychic::FS` take **real POSIX paths**. So `Web.cpp`'s static-asset registration must say `/littlefs/w` while every other line in the codebase says `/w`. That is two path namespaces coexisting in one file, distinguishable only by which function you are calling — exactly the kind of thing that works until someone passes a path across the boundary and gets a silent 404 on a device with no serial console.

The better fix is to **change the six constants** — `SMOLBASE_WWW_DIR`, `SMOLBASE_SETTINGS_PATH`, and the handful of `snprintf` format strings that build paths — so there is one path namespace and it is the real one. That is a smaller edit than the prefixing logic, and it removes a failure mode instead of adding one.

(A root mount, `base_path = ""`, would dodge this entirely if ESP-IDF's VFS permits it. Unverified — worth five minutes before accepting the constant edit.)

## Verdict

Worth doing, at about 50 lines:

- **Path-op wrappers (~12 lines)** — prevents 30 chances at a silent condition inversion. Highest value per line in the whole migration.
- **RAII `File` (~35 lines)** — closes a reachable descriptor-leak class against a hard 10-descriptor ceiling.
- **Local `Dir` in `AssetUpdate.cpp`** — not a core abstraction.
- **No path translation.** Fix the constants instead.

So: yes to the file handle, yes to something you probably were not thinking of (the bool-semantics wrappers, which are the cheaper win), no to the thing that looked like the biggest win. This is a *file access* helper, not a `LittleFS` compatibility layer — consistent with the scope decision that ruled out emulating `fs::FS`.

## Next step

The spike already mounts the live volume and lists `/w` (check 2), so folding `sb_fs.h` and a local `Dir` into it would compile and exercise both against real hardware before any core module is touched — turning this sketch into a verified component for roughly an hour's work.
