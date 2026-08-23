// GitHub release OTA: version check and firmware-only flash from public releases.
// GET  /api/update/check      — GitHub API; returns {current,latest,upToDate}
// GET  /api/update/ghprogress — live state for the settings UI polling loop
// POST /api/update/github     — body {tag}; spawns the download task, responds 200
//
// Download: esp_https_ota (raw begin/perform/finish) on the deterministic
// release URL https://github.com/<repo>/releases/download/<tag>/<asset>.bin.
// esp_http_client follows the 302 to the CDN natively and reads block in
// select(), so no manual redirect capture and no TCP-window stall workaround
// (see docs/research/ghupdate-esp-https-ota.md and wayfinder map #112).
//
// The task is pinned to Core 0: WiFi/lwIP live there and HTTPS from Core 1
// crashes (~8 s WDT; root cause unpinned — ghupdate-mission-notes.md pitfall 1).
//
// Heap discipline (map #112, tickets #116/#119): the TLS handshake against the
// GitHub CDN needs its RSA-4096 chain verified with real heap headroom — the
// 2026-08 field break was MPI allocation failure at ~48 KB free, not a trust
// problem. So: the task waits for the OtaStarting app suspension to land
// (heap plateau) before any TLS work, and only one TLS context ever lives at
// a time (esp_https_ota owns a single client through the redirect).
#include "GhUpdate.h"
#include "AssetUpdate.h"
#include "Events.h"
#include "Http.h"
#include "Net.h"
#include "Platform.h"
#include "smolbase_config.h"
#include <ArduinoJson.h>
#include <PsychicHttp.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>

static const char* const GH_REPO = "sweetlilmre/smolbase";

// Release asset this build self-updates from: <prefix>-<tag>.bin. Each env
// overrides via build_flags so a weatherclock device never flashes the
// smolbase image (CI ships one firmware/littlefs pair per env).
#ifndef SMOLBASE_FW_ASSET_PREFIX
#define SMOLBASE_FW_ASSET_PREFIX "smolbase-firmware"
#endif
static const char* const GH_FW_PREFIX = SMOLBASE_FW_ASSET_PREFIX;

namespace GhUpdate {


struct Progress {
  enum State : uint8_t { Idle, Downloading, Done, Error };
  volatile State  state        = Idle;
  volatile size_t bytesWritten = 0;
  volatile size_t totalBytes   = 0;
  volatile int    filesDone    = 0;
  volatile int    filesTotal   = 0;
  char            phase[10]    = {};   // "firmware" | "assets"
  char            errorMsg[96] = {};
};
static Progress s_progress;
static volatile bool s_inFlight = false;
static char s_tag[32] = {}; // written by the POST handler before the task spawns

static std::string detectLatestTag() {
  const std::string url =
      std::string("https://api.github.com/repos/") + GH_REPO + "/releases/latest";
  JsonDocument filter;
  filter["tag_name"] = true;
  JsonDocument doc;
  const Http::Header hdrs[] = {{"Accept", "application/vnd.github+json"}};
  Http::Request rq;
  rq.url = url.c_str();
  rq.filter = &filter;
  rq.headers = hdrs;
  rq.headerCount = 1;
  // Streamed, not buffered: this response is tens of KB (see Http.h).
  if (!Http::json(rq, doc).ok) return {};
  return doc["tag_name"].as<std::string>();
}

// ---- version comparison -----------------------------------------------------
// Tags are "vX.Y.Z" with an optional "-suffix" for unreleased builds. A plain
// string compare cannot answer "is there anything newer", which is the only
// question /api/update/check is actually asking.
struct Ver {
  int major = -1, minor = 0, patch = 0;
  bool pre = false; // has a "-suffix"
  bool valid() const { return major >= 0; }
};

static Ver parseVer(const std::string& s) {
  Ver v;
  const char* p = s.c_str();
  if (*p == 'v' || *p == 'V') ++p;
  int n = 0;
  if (sscanf(p, "%d.%d.%d%n", &v.major, &v.minor, &v.patch, &n) != 3) return Ver{};
  v.pre = p[n] == '-';
  return v;
}

// -1 / 0 / +1. Semver precedence: a pre-release sorts BEFORE the same X.Y.Z, so
// 0.4.0-dev < 0.4.0 while still being greater than 0.3.3.
static int cmpVer(const Ver& a, const Ver& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.pre != b.pre) return a.pre ? -1 : 1;
  return 0;
}

static bool failOta(const char* msg) {
  printf("[ghupdate] failed: %s\n", msg);
  strlcpy(s_progress.errorMsg, msg, sizeof(s_progress.errorMsg));
  s_progress.state = Progress::Error;
  s_inFlight = false;
  return false;
}

static esp_err_t httpClientInitCb(esp_http_client_handle_t h) {
  esp_http_client_set_header(h, "User-Agent", "smolbase-esp32");
  return ESP_OK;
}

// Wait for the OtaStarting suspension to free the app's memory before TLS:
// heap must be stable for 500 ms (or 3 s cap). Worth ~15 KB at the handshake.
static void waitForHeapPlateau() {
  uint32_t t0 = Platform::millis(), stableSince = Platform::millis();
  uint32_t last = Platform::freeHeap();
  while (Platform::millis() - t0 < 3000) {
    vTaskDelay(pdMS_TO_TICKS(100));
    uint32_t now = Platform::freeHeap();
    if (now > last + 1024) { last = now; stableSince = Platform::millis(); }
    else if (Platform::millis() - stableSince >= 500) break;
  }
}

// All network + flash work; plain function so locals unwind before vTaskDelete.
// Full-flow design: wayfinder #122 (staged tar, version-named /w backup).
static void assetProgress(int done, int total) {
  s_progress.filesDone  = done;
  s_progress.filesTotal = total;
}

static bool s_rebootAfter = false;

static bool downloadImpl(const char* tag) {
  waitForHeapPlateau();

  // Same-tag POST = asset reinstall: skip the firmware stage entirely.
  bool sameVer = (tag[0] == 'v' && strcmp(tag + 1, SMOLBASE_FW_VERSION) == 0);
  s_rebootAfter = !sameVer;
  char m[96];

  // The release's asset digest gates the tar's content integrity; fetch it
  // first (one small TLS session, closed before anything else opens).
  char digest[65];
  if (!AssetUpdate::fetchAssetDigest(tag, digest, sizeof(digest), m, sizeof(m)))
    return failOta(m);
  AssetUpdate::sweepStaleStaging();

  // Tar BEFORE firmware: the esp_https_ota handle keeps its HTTP client and
  // TLS buffers alive until finish/abort, and two TLS contexts never fit on
  // this heap (#119). Downloading the tar first means its session is fully
  // closed before the OTA session opens; the fs is still only mutated after
  // both downloads are complete and verified.
  strlcpy(s_progress.phase, "assets", sizeof(s_progress.phase));
  s_progress.state = Progress::Downloading;
  if (!AssetUpdate::downloadTar(tag, digest, m, sizeof(m)))
    return failOta(m);

  esp_https_ota_handle_t handle = nullptr;

  if (!sameVer) {
    strlcpy(s_progress.phase, "firmware", sizeof(s_progress.phase));

    char url[160];
    snprintf(url, sizeof(url), "https://github.com/%s/releases/download/%s/%s-%s.bin",
             GH_REPO, tag, GH_FW_PREFIX, tag);
    printf("[ghupdate] pulling %s (heap=%u)\n", url, Platform::freeHeap());

    esp_http_client_config_t http = {};
    http.url               = url;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms        = 30000;
    http.buffer_size       = 4096; // rx buffer; doubles as the OTA upgrade buffer size
    http.buffer_size_tx    = 2048; // request line must fit the ~1.2 KB CDN redirect URL

    esp_https_ota_config_t ota = {};
    ota.http_config         = &http;
    ota.http_client_init_cb = httpClientInitCb;
    // partial_http_download stays off: its HEAD preflight dies on the 302.

    // A user-initiated OTA implies the running image is functional (it booted,
    // joined WiFi, and served the UI), so confirm it if the #76 rollback guard
    // hasn't yet — esp_ota_begin refuses to start while PENDING_VERIFY.
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t imgState;
    if (esp_ota_get_state_partition(running, &imgState) == ESP_OK &&
        imgState == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
    }

    esp_err_t err = esp_https_ota_begin(&ota, &handle);
    if (err != ESP_OK) {
      snprintf(m, sizeof(m), "begin: %s (heap=%u max=%u)",
               esp_err_to_name(err), Platform::freeHeap(), Platform::largestFreeBlock());
      AssetUpdate::sweepStaleStaging();
      return failOta(m);
    }

    int total = esp_https_ota_get_image_size(handle);
    s_progress.totalBytes = total > 0 ? (size_t)total : 0;
    s_progress.state      = Progress::Downloading;

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
      int got = esp_https_ota_get_image_len_read(handle);
      if (got >= 0) s_progress.bytesWritten = (size_t)got;
    }
    int got = esp_https_ota_get_image_len_read(handle);
    if (got >= 0) s_progress.bytesWritten = (size_t)got;

    if (err != ESP_OK) {
      snprintf(m, sizeof(m), "perform: %s at %u/%u", esp_err_to_name(err),
               (unsigned)s_progress.bytesWritten, (unsigned)s_progress.totalBytes);
      esp_https_ota_abort(handle);
      AssetUpdate::sweepStaleStaging();
      return failOta(m);
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
      esp_https_ota_abort(handle);
      AssetUpdate::sweepStaleStaging();
      return failOta("incomplete download");
    }
    // Firmware fully staged — NOT finalized until the assets are in place.
  }

  strlcpy(s_progress.phase, "assets", sizeof(s_progress.phase));

  if (sameVer) {
    if (!AssetUpdate::applyTarInPlace(assetProgress, m, sizeof(m)))
      return failOta(m);
    printf("[ghupdate] assets reinstalled\n");
    return true; // no reboot: firmware unchanged
  }

  if (!AssetUpdate::applyTarWithBackup(assetProgress, m, sizeof(m))) {
    esp_https_ota_abort(handle); // /w already restored by applyTarWithBackup
    return failOta(m);
  }

  esp_err_t err = esp_https_ota_finish(handle); // validates and switches the boot partition
  if (err != ESP_OK) {
    // Old firmware keeps running: undo the asset swap now rather than at boot.
    AssetUpdate::bootHeal();
    snprintf(m, sizeof(m), "finish: %s", esp_err_to_name(err));
    return failOta(m);
  }

  printf("[ghupdate] flashed %u bytes + %d asset files\n",
                (unsigned)s_progress.bytesWritten, s_progress.filesDone);
  return true;
}

static void downloadTask(void* arg) {
  if (downloadImpl(static_cast<const char*>(arg))) {
    s_progress.state = Progress::Done;
    s_inFlight = false;
    if (s_rebootAfter) {
      printf("[ghupdate] done - restarting\n");
      vTaskDelay(pdMS_TO_TICKS(3000)); // let the settings page poll the final "done"
      Net::restartToApply();
    }
  }
  // On failure, failOta() already set state/errorMsg and cleared s_inFlight.
  vTaskDelete(nullptr);
}

void registerRoutes(PsychicHttpServer& server) {
  server.on("/api/update/check", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    if (s_inFlight)
      return res->send(409, "application/json", "{\"error\":\"update in progress\"}");
    const std::string latest = detectLatestTag();
    if (latest.empty())
      return res->send(503, "application/json", "{\"error\":\"could not reach GitHub releases\"}");
    const std::string current = std::string("v") + SMOLBASE_FW_VERSION;
    // upToDate means "no NEWER release exists" — NOT "the strings match".
    // Equality was the old test, and it reported a device running something
    // newer than the latest release as out of date, which made the UI offer a
    // DOWNGRADE (and GhUpdate would happily flash it, assets included).
    const Ver cur = parseVer(current);
    const Ver lat = parseVer(latest);
    const bool comparable = cur.valid() && lat.valid();
    const bool ahead = comparable && cmpVer(cur, lat) > 0;
    // Unparseable tags fall back to equality: conservative, and preserves the
    // old behaviour for anything that is not vX.Y.Z.
    const bool upToDate = comparable ? cmpVer(lat, cur) <= 0 : (latest == current);
    const std::string out = "{\"current\":\"" + current + "\",\"latest\":\"" + latest +
                            "\",\"upToDate\":" + (upToDate ? "true" : "false") +
                            ",\"ahead\":" + (ahead ? "true" : "false") + "}";
    return res->send(200, "application/json", out.c_str());
  });

  server.on("/api/update/ghprogress", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    static const char* const STATES[] = { "idle", "downloading", "done", "error" };
    char buf[280];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"phase\":\"%s\",\"bytesWritten\":%u,\"totalBytes\":%u,"
             "\"filesDone\":%d,\"filesTotal\":%d,\"error\":\"%s\"}",
             STATES[(int)s_progress.state],
             s_progress.phase,
             (unsigned)s_progress.bytesWritten,
             (unsigned)s_progress.totalBytes,
             s_progress.filesDone,
             s_progress.filesTotal,
             s_progress.errorMsg);
    return res->send(200, "application/json", buf);
  });

  // Spawns the Core 0 download task directly — the httpd task is already on
  // Core 0 and serves one request at a time, so a plain flag serializes this.
  server.on("/api/update/github", HTTP_POST, [](PsychicRequest* req, PsychicResponse* res) {
    if (s_inFlight)
      return res->send(409, "application/json", "{\"error\":\"update already in progress\"}");
    JsonDocument doc;
    if (deserializeJson(doc, req->body()) != DeserializationError::Ok)
      return res->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    String tag = doc["tag"] | String("");
    if (tag.isEmpty() || !tag.startsWith("v"))
      return res->send(400, "application/json", "{\"error\":\"missing or invalid tag\"}");

    s_inFlight = true;
    s_progress = {};
    strlcpy(s_tag, tag.c_str(), sizeof(s_tag));
    Events::post(SysEvent::OtaStarting); // suspends the app; the task waits for the heap plateau

    // 8 KB stack: measured high-water was ~4.9 KB incl. TLS handshake depth
    // (ticket #116); esp_https_ota's own buffers are heap-allocated.
    if (xTaskCreatePinnedToCore(downloadTask, "ghota", 8192, s_tag, 5, nullptr, 0) != pdPASS) {
      s_inFlight = false;
      s_progress.state = Progress::Error;
      strlcpy(s_progress.errorMsg, "task create failed", sizeof(s_progress.errorMsg));
      return res->send(500, "application/json", "{\"error\":\"task create failed\"}");
    }
    return res->send(200, "application/json",
                     ("{\"ok\":true,\"tag\":\"" + tag + "\"}").c_str());
  });
}

} // namespace GhUpdate
