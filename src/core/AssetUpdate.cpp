// See AssetUpdate.h. Mechanics per docs/research/assets-tar-mechanics.md:
// hand-rolled read-only ustar walker (microtar rejected: unmaintained,
// seek-dependent, overflow-prone); LittleFS directory rename is atomic and
// power-loss safe (lfs_rename commits the move as one metadata commit);
// content integrity via GitHub's per-asset sha256 digest streamed during
// download (tar header checksums cover headers only).
#include "AssetUpdate.h"
#include "Fs.h"
#include "Http.h"
#include "smolbase_config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <mbedtls/sha256.h>

static const char* const GH_REPO = "sweetlilmre/smolbase";

// Release asset carrying this build's web assets: <prefix>-<tag>.tar.
// Envs override via build_flags, mirroring SMOLBASE_FW_ASSET_PREFIX.
#ifndef SMOLBASE_ASSETS_PREFIX
#define SMOLBASE_ASSETS_PREFIX "smolbase-assets"
#endif

static const char* const kStagingTar = SMOLBASE_FS_MOUNT "/assets.tar";
static const char* const kWebDir     = SMOLBASE_FS_MOUNT "/w";

namespace AssetUpdate {

static void setErr(char* buf, size_t len, const char* fmt, ...) {
  if (!buf || !len) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, len, fmt, ap);
  va_end(ap);
}

static void backupName(char* out, size_t len) {
  snprintf(out, len, SMOLBASE_FS_MOUNT "/w.v%s", SMOLBASE_FW_VERSION);
}

// Remove a flat directory (files only — /w never holds subdirs) then the dir.
static bool removeDirRecursive(const char* path) {
  if (!Fs::isDir(path)) return false;
  Fs::Dir dir(path);
  if (!dir) return false;
  // Collect names first: deleting while iterating trips some FS iterators.
  std::string names[24];
  int n = 0;
  Fs::Dir::Entry e;
  while (n < 24 && dir.next(e)) names[n++] = e.path;
  dir.close();
  for (int i = 0; i < n; i++) Fs::remove(names[i].c_str());
  return Fs::rmdir(path);
}

// Find a "/w.<something>" backup dir at the fs root; returns true and fills
// out (e.g. "/w.v0.3.2") if one exists.
static bool findBackupDir(char* out, size_t len) {
  Fs::Dir root(SMOLBASE_FS_MOUNT "/");
  if (!root) return false;
  bool found = false;
  Fs::Dir::Entry ent;
  while (root.next(ent)) {
    // Match on the BASENAME, not the full path: the old strncmp(p, "/w.", 3)
    // assumed the volume was mounted at the root and would silently stop
    // matching once a mount prefix was in play.
    const char* p = ent.path.c_str(); // e.g. "/littlefs/w.v0.3.2"
    bool isDir = ent.isDir;
    if (isDir && strncmp(ent.name.c_str(), "w.", 2) == 0) {
      strlcpy(out, p, len);
      found = true;
      break;
    }
  }
  root.close();
  return found;
}

// ---- GitHub digest lookup -------------------------------------------------


bool fetchAssetDigest(const char* tag, char* outHex, size_t outHexLen, char* errBuf, size_t errLen) {
  char assetName[80];
  snprintf(assetName, sizeof(assetName), "%s-%s.tar", SMOLBASE_ASSETS_PREFIX, tag);

  JsonDocument filter;
  filter["assets"][0]["name"]   = true;
  filter["assets"][0]["digest"] = true;
  JsonDocument doc;
  const std::string url =
      std::string("https://api.github.com/repos/") + GH_REPO + "/releases/tags/" + tag;
  const Http::Header hdrs[] = {{"Accept", "application/vnd.github+json"}};
  Http::Request rq;
  rq.url = url.c_str();
  rq.filter = &filter;
  rq.headers = hdrs;
  rq.headerCount = 1;
  rq.timeoutMs = 15000;
  // Streamed, not buffered: this response is tens of KB (see Http.h).
  Http::Result hr = Http::json(rq, doc);
  if (!hr.ok) {
    setErr(errBuf, errLen, "digest: HTTP %d %s", hr.status, hr.err);
    return false;
  }
  for (JsonObject a : doc["assets"].as<JsonArray>()) {
    if (String(a["name"] | "") != assetName) continue;
    const char* d = a["digest"] | "";
    if (strncmp(d, "sha256:", 7) != 0 || strlen(d + 7) != 64) {
      setErr(errBuf, errLen, "digest: missing on %s", assetName);
      return false;
    }
    strlcpy(outHex, d + 7, outHexLen);
    return true;
  }
  setErr(errBuf, errLen, "no %s in release", assetName);
  return false;
}

// ---- tar download with streamed SHA-256 -----------------------------------

bool downloadTar(const char* tag, const char* expectedHex, char* errBuf, size_t errLen) {
  char url[176];
  snprintf(url, sizeof(url), "https://github.com/%s/releases/download/%s/%s-%s.tar",
           GH_REPO, tag, SMOLBASE_ASSETS_PREFIX, tag);

  Fs::remove(kStagingTar);
  Fs::File out(kStagingTar, "w");
  if (!out) {
    setErr(errBuf, errLen, "tar: staging open failed");
    return false;
  }

  esp_http_client_config_t cfg = {};
  cfg.url               = url;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms        = 30000;
  cfg.buffer_size       = 2048;
  cfg.buffer_size_tx    = 2048; // request line must fit the ~1.2 KB CDN redirect URL
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    out.close();
    Fs::remove(kStagingTar);
    setErr(errBuf, errLen, "tar: client init failed");
    return false;
  }
  esp_http_client_set_header(client, "User-Agent", "smolbase-esp32");

  bool ok = false;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  do {
    // open+fetch follows the 302 to the CDN internally (native redirects).
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      setErr(errBuf, errLen, "tar: open %s", esp_err_to_name(err));
      break;
    }
    int64_t total = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    // Drain redirect bodies until a final status arrives.
    int hops = 0;
    while ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && hops++ < 5) {
      esp_http_client_set_redirection(client);
      err = esp_http_client_open(client, 0);
      if (err != ESP_OK) break;
      total  = esp_http_client_fetch_headers(client);
      status = esp_http_client_get_status_code(client);
    }
    if (err != ESP_OK || status != 200 || total <= 0) {
      setErr(errBuf, errLen, "tar: HTTP %d len=%d", status, (int)total);
      break;
    }

    uint8_t buf[1024];
    int64_t got = 0;
    bool ioFail = false;
    while (got < total) {
      int rd = esp_http_client_read(client, (char*)buf, sizeof(buf));
      if (rd <= 0) { ioFail = true; break; }
      mbedtls_sha256_update(&sha, buf, rd);
      if (out.write(buf, rd) != (size_t)rd) { ioFail = true; break; }
      got += rd;
    }
    if (ioFail || got != total) {
      setErr(errBuf, errLen, "tar: read %d/%d", (int)got, (int)total);
      break;
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    if (strcasecmp(hex, expectedHex) != 0) {
      setErr(errBuf, errLen, "tar: sha256 mismatch");
      break;
    }
    ok = true;
  } while (false);

  mbedtls_sha256_free(&sha);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  out.close();
  if (!ok) Fs::remove(kStagingTar);
  return ok;
}

// ---- ustar walker ----------------------------------------------------------
// POSIX ustar: 512-byte header blocks; octal size at offset 124 (12 bytes,
// space/NUL-terminated); checksum at 148 (8) = unsigned byte sum of the
// header with the checksum field as spaces; typeflag at 156; magic "ustar"
// at 257; two all-zero blocks terminate. CI ships --format=ustar with flat
// short names, so anything fancier is rejected outright.

struct TarEntry {
  char   name[101];
  size_t size;
};

// Reads the next FILE entry header from `tar`, silently skipping directory
// entries (tar -C dir . emits the "./" dir itself as typeflag '5').
// Returns 1 entry, 0 clean end, -1 error.
static int readHeader(Fs::File& tar, TarEntry& e, char* errBuf, size_t errLen) {
 nextHeader:
  uint8_t h[512];
  if (tar.readBytes(h, 512) != 512) {
    setErr(errBuf, errLen, "tar: truncated header");
    return -1;
  }
  bool allZero = true;
  for (int i = 0; i < 512 && allZero; i++) allZero = (h[i] == 0);
  if (allZero) {
    uint8_t h2[512];
    if (tar.readBytes(h2, 512) != 512) {
      setErr(errBuf, errLen, "tar: missing 2nd terminator");
      return -1;
    }
    return 0;
  }
  if (memcmp(h + 257, "ustar", 5) != 0) {
    setErr(errBuf, errLen, "tar: bad magic");
    return -1;
  }
  // Checksum: unsigned sum with the chksum field (148..155) as spaces.
  uint32_t sum = 0;
  for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
  uint32_t stored = 0;
  for (int i = 148; i < 156; i++) {
    uint8_t c = h[i];
    if (c == ' ' || c == 0) continue;
    if (c < '0' || c > '7') { setErr(errBuf, errLen, "tar: bad chksum"); return -1; }
    stored = stored * 8 + (c - '0');
  }
  if (sum != stored) {
    setErr(errBuf, errLen, "tar: chksum mismatch");
    return -1;
  }
  char type = (char)h[156];
  if (type != '0' && type != '\0' && type != '5') {
    setErr(errBuf, errLen, "tar: entry type '%c'", type ? type : '0');
    return -1;
  }
  // Size: bounded octal parse of bytes 124..135.
  size_t size = 0;
  for (int i = 124; i < 136; i++) {
    uint8_t c = h[i];
    if (c == ' ' || c == 0) break;
    if (c < '0' || c > '7') { setErr(errBuf, errLen, "tar: bad size"); return -1; }
    size = size * 8 + (c - '0');
  }
  if (type == '5') { // directory entry: skip its (normally zero) content
    size_t pad = (512 - (size % 512)) % 512;
    if (size + pad > 0 && !tar.seek(tar.position() + size + pad)) {
      setErr(errBuf, errLen, "tar: truncated dir entry");
      return -1;
    }
    goto nextHeader;
  }
  // Name: NUL-pad copy (the 100-byte field may legally lack a terminator);
  // strip a leading "./" from `tar -C dir .` style archives.
  char raw[101];
  memcpy(raw, h, 100);
  raw[100] = 0;
  const char* name = raw;
  if (name[0] == '.' && name[1] == '/') name += 2;
  if (name[0] == 0 || strchr(name, '/') != nullptr || strlen(name) > 64) {
    setErr(errBuf, errLen, "tar: bad name '%.20s'", raw);
    return -1;
  }
  strlcpy(e.name, name, sizeof(e.name));
  e.size = size;
  return 1;
}

static bool skipPadding(Fs::File& tar, size_t size) {
  size_t pad = (512 - (size % 512)) % 512;
  return pad == 0 || tar.seek(tar.position() + pad);
}

int validateTar(char* errBuf, size_t errLen) {
  Fs::File tar(kStagingTar, "r");
  if (!tar) {
    setErr(errBuf, errLen, "tar: staging missing");
    return -1;
  }
  int files = 0;
  TarEntry e;
  int r;
  while ((r = readHeader(tar, e, errBuf, errLen)) == 1) {
    if (!tar.seek(tar.position() + e.size) || !skipPadding(tar, e.size)) {
      setErr(errBuf, errLen, "tar: truncated content");
      tar.close();
      return -1;
    }
    files++;
  }
  tar.close();
  if (r != 0) return -1;
  if (files == 0) {
    setErr(errBuf, errLen, "tar: empty archive");
    return -1;
  }
  return files;
}

// Extract every entry into destDir. If viaTmp, write <dest>.tmp then rename
// over (in-place reinstall); otherwise write directly (fresh dir after the
// backup rename — a torn extract is healed by bootHeal's restore).
static bool extractTo(const char* destDir, bool viaTmp, int totalFiles,
                      FileProgressCb cb, char* errBuf, size_t errLen) {
  Fs::File tar(kStagingTar, "r");
  if (!tar) {
    setErr(errBuf, errLen, "tar: staging missing");
    return false;
  }
  TarEntry e;
  int r, done = 0;
  while ((r = readHeader(tar, e, errBuf, errLen)) == 1) {
    char dst[96], tmp[100];
    snprintf(dst, sizeof(dst), "%s/%s", destDir, e.name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    const char* writePath = viaTmp ? tmp : dst;
    Fs::File out(writePath, "w");
    if (!out) {
      setErr(errBuf, errLen, "extract: open %s", writePath);
      tar.close();
      return false;
    }
    size_t remaining = e.size;
    uint8_t buf[1024];
    while (remaining > 0) {
      size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
      if (tar.readBytes(buf, chunk) != chunk || out.write(buf, chunk) != chunk) {
        out.close();
        Fs::remove(writePath);
        tar.close();
        setErr(errBuf, errLen, "extract: io %s", e.name);
        return false;
      }
      remaining -= chunk;
    }
    out.close();
    if (viaTmp) {
      Fs::remove(dst); // rename requires the destination absent
      if (!Fs::rename(tmp, dst)) {
        tar.close();
        setErr(errBuf, errLen, "extract: rename %s", e.name);
        return false;
      }
    }
    if (!skipPadding(tar, e.size)) {
      tar.close();
      setErr(errBuf, errLen, "tar: bad padding");
      return false;
    }
    done++;
    if (cb) cb(done, totalFiles);
  }
  tar.close();
  return r == 0;
}

bool applyTarWithBackup(FileProgressCb cb, char* errBuf, size_t errLen) {
  int files = validateTar(errBuf, errLen);
  if (files < 0) return false;

  char bak[24];
  backupName(bak, sizeof(bak));
  removeDirRecursive(bak); // leftover from an interrupted run
  if (!Fs::rename(kWebDir, bak)) {
    setErr(errBuf, errLen, "backup rename failed");
    return false;
  }
  Fs::mkdir(kWebDir);
  if (!extractTo(kWebDir, false, files, cb, errBuf, errLen)) {
    // Restore the exact old set: we are still running the old firmware.
    removeDirRecursive(kWebDir);
    Fs::rename(bak, kWebDir);
    Fs::remove(kStagingTar);
    return false;
  }
  Fs::remove(kStagingTar);
  return true;
}

bool applyTarInPlace(FileProgressCb cb, char* errBuf, size_t errLen) {
  int files = validateTar(errBuf, errLen);
  if (files < 0) return false;
  if (!extractTo(kWebDir, true, files, cb, errBuf, errLen)) {
    Fs::remove(kStagingTar);
    return false;
  }
  // Orphan sweep: the tar is authoritative for /w.
  Fs::File tar(kStagingTar, "r");
  std::string keep[24];
  int nKeep = 0;
  if (tar) {
    TarEntry e;
    while (nKeep < 24 && readHeader(tar, e, nullptr, 0) == 1) {
      keep[nKeep++] = std::string(kWebDir) + "/" + e.name;
      if (!tar.seek(tar.position() + e.size) || !skipPadding(tar, e.size)) break;
    }
    tar.close();
  }
  Fs::Dir dir(kWebDir);
  std::string doomed[24];
  int nDoom = 0;
  if (dir) {
    Fs::Dir::Entry de;
    while (nDoom < 24 && dir.next(de)) {
      bool listed = false;
      for (int i = 0; i < nKeep && !listed; i++) listed = (de.path == keep[i]);
      if (!listed) doomed[nDoom++] = de.path;
    }
    dir.close();
  }
  for (int i = 0; i < nDoom; i++) Fs::remove(doomed[i].c_str());
  Fs::remove(kStagingTar);
  return true;
}

void sweepStaleStaging() {
  Fs::remove(kStagingTar);
  char bak[32];
  if (findBackupDir(bak, sizeof(bak))) removeDirRecursive(bak);
}

void bootHeal() {
  char bak[32], mine[24];
  if (!findBackupDir(bak, sizeof(bak))) {
    Fs::remove(kStagingTar);
    return;
  }
  backupName(mine, sizeof(mine));
  if (strcmp(bak, mine) == 0) {
    // The backup holds *this* version's assets: we were rolled back, or the
    // update tore before finalize. Restore the exact old set.
    Serial.printf("[assets] boot heal: restoring %s\n", bak);
    removeDirRecursive(kWebDir);
    Fs::rename(bak, kWebDir);
  }
  // A backup for a different version is deleted at onImageConfirmed(), not
  // here — deleting it before the 30 s guard passes would strand a rollback.
  Fs::remove(kStagingTar);
}

void onImageConfirmed() {
  char bak[32], mine[24];
  if (findBackupDir(bak, sizeof(bak))) {
    backupName(mine, sizeof(mine));
    if (strcmp(bak, mine) != 0) {
      Serial.printf("[assets] image confirmed - deleting %s\n", bak);
      removeDirRecursive(bak);
    }
  }
  Fs::remove(kStagingTar);
}

} // namespace AssetUpdate
