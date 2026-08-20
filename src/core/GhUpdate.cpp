// GitHub release OTA: version check and firmware-only flash from public releases.
// GET  /api/update/check      — GitHub API; returns {current,latest,upToDate}
// GET  /api/update/ghprogress — live state for the settings UI polling loop
// POST /api/update/github     — body {tag}; queues download, responds 200 immediately
//
// Download runs on a dedicated Core 0 task (downloadTask).
//
// Why Core 0? WiFi and lwIP run on Core 0. HTTPS connections require coordination
// with those drivers, and BundleClient connections reliably work from Core 0
// tasks (the httpd task — also Core 0 — proves this). The Arduino loop() is
// Core 1; making HTTPS from there crashes the device at ~8 s (stack or lwIP IPC
// timing issue, root cause unclear). Custom Core 1 tasks also fail TCP. Core 0
// pinning is the minimal, proven fix.
//
// downloadTask stack: 16 KB — TLS buffers are heap-allocated; only call depth
// needs stack. Write buffer is also heap-alloc'd to avoid tipping this over.
#include "GhUpdate.h"
#include "Events.h"
#include "Net.h"
#include "smolbase_config.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <PsychicHttp.h>
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* const GH_REPO      = "sweetlilmre/smolbase";
static const char* const GH_FW_PREFIX = "smolbase-firmware";

namespace GhUpdate {

class BundleClient : public NetworkClientSecure {
public:
  BundleClient() { attach_ssl_certificate_bundle(sslclient.get(), true); _use_ca_bundle = true; }
};

struct Progress {
  enum State : uint8_t { Idle, Downloading, Done, Error };
  volatile State  state        = Idle;
  volatile size_t bytesWritten = 0;
  size_t          totalBytes   = 0;
  char            errorMsg[80] = {};
};
static Progress s_progress;
static volatile bool s_inFlight = false;

// Cross-core handoff: POST handler (Core 0 httpd task) writes tag then flag.
// tick() (Core 1 loop) spawns the Core 0 download task when it sees the flag.
static char             s_pendingBuf[32] = {};
static volatile bool    s_hasPending     = false;
static volatile uint32_t s_tickCount     = 0; // visible in ghprogress for diagnostics

static String detectLatestTag() {
  BundleClient tls;
  HTTPClient http;
  http.setTimeout(10000);
  http.setConnectTimeout(10000);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("User-Agent", "smolbase-esp32");
  if (!http.begin(tls, String("https://api.github.com/repos/") + GH_REPO + "/releases/latest"))
    return "";
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return ""; }
  JsonDocument filter;
  filter["tag_name"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, *http.getStreamPtr(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err != DeserializationError::Ok) return "";
  return doc["tag_name"] | String("");
}

// Inner function: all network + flash work. Returns true on success.
// MUST be called from downloadTask so that BundleClient/HTTPClient destructors
// run before vTaskDelete — vTaskDelete skips C++ cleanup, causing TLS heap leaks.
static bool downloadImpl(const char* tag) {
  // Call Update.begin() FIRST — before any TLS/HTTP allocations.
  // Update.begin() internally allocates a 4 KB sector buffer (_buffer).
  // That allocation fails with "No Error" (nothrow path, no _error set)
  // once a BundleClient (~40 KB) is live alongside the 16 KB task stack.
  // Moving begin() here gives it ~86 KB of clean heap to work with.
  if (Update.isRunning()) Update.abort();
  if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    char m[80];
    snprintf(m, sizeof(m), "begin: %s (free=%u max=%u)",
             Update.errorString(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    strlcpy(s_progress.errorMsg, m, sizeof(s_progress.errorMsg));
    s_progress.state = Progress::Error;
    s_inFlight = false;
    return false;
  }

  // begin() succeeded — all failure paths below must abort Update.
  auto fail = [](const char* msg) -> bool {
    Serial.printf("[ghupdate] failed: %s\n", msg);
    if (Update.isRunning()) Update.abort();
    strlcpy(s_progress.errorMsg, msg, sizeof(s_progress.errorMsg));
    s_progress.state = Progress::Error;
    s_inFlight = false;
    return false;
  };

  // Single BundleClient across all three steps — using HTTPC_DISABLE_FOLLOW_REDIRECTS
  // for step 2 means HTTPClient never does its own TLS reconnect, which was causing
  // 0-byte body delivery when it internally followed github.com → CDN.
  BundleClient tls;
  HTTPClient   http;
  http.addHeader("User-Agent", "smolbase-esp32");
  http.setConnectTimeout(10000);

  // Step 1: asset API URL from release JSON
  String assetApiUrl;
  {
    http.setTimeout(15000);
    http.addHeader("Accept", "application/vnd.github+json");
    if (!http.begin(tls, String("https://api.github.com/repos/") + GH_REPO +
                    "/releases/tags/" + tag)) {
      return fail("step1 begin failed");
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      char m[32]; snprintf(m, sizeof(m), "step1 HTTP %d", code);
      http.end(); return fail(m);
    }
    JsonDocument filter;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["url"]  = true;
    JsonDocument body;
    deserializeJson(body, *http.getStreamPtr(), DeserializationOption::Filter(filter));
    http.end();
    String fwName = String(GH_FW_PREFIX) + "-" + tag + ".bin";
    for (JsonObject asset : body["assets"].as<JsonArray>()) {
      if (String(asset["name"] | "") == fwName) {
        assetApiUrl = String(asset["url"] | "");
        break;
      }
    }
  }
  if (assetApiUrl.isEmpty()) { return fail("firmware asset not found"); }

  // Step 2: CDN URL via asset redirect (manual — no HTTPC_FORCE_FOLLOW_REDIRECTS)
  String cdnUrl;
  {
    http.setTimeout(15000);
    http.addHeader("Accept", "application/octet-stream");
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(tls, assetApiUrl)) { return fail("step2 begin failed"); }
    int code = http.GET();
    if (code == HTTP_CODE_FOUND || code == 301) {
      cdnUrl = http.getLocation();
    } else {
      char m[32]; snprintf(m, sizeof(m), "step2 HTTP %d", code);
      http.end(); return fail(m);
    }
    http.end();
  }
  if (cdnUrl.isEmpty()) { return fail("CDN URL empty"); }
  Serial.printf("[ghupdate] CDN: %.80s...\n", cdnUrl.c_str());
  Serial.printf("[ghupdate] heap before CDN: %u\n", ESP.getFreeHeap());

  // Step 3: stream CDN binary directly into Update flash writer.
  // HTTPClient::setTimeout takes uint16_t — max 65535 ms; use 60 s.
  // tls.setTimeout (Stream) takes unsigned long, so full 120 s there.
  // No vTaskDelay in the inner loop: FreeRTOS preemption keeps other tasks alive,
  // and yielding after every 4 KB chunk was stalling TCP ACK delivery.
  http.setTimeout(60000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(tls, cdnUrl)) { return fail("CDN begin failed"); }

  int code = http.GET();
  Serial.printf("[ghupdate] CDN HTTP %d heap=%u\n", code, ESP.getFreeHeap());
  if (code != HTTP_CODE_OK) {
    char m[40]; snprintf(m, sizeof(m), "CDN HTTP %d", code);
    http.end(); return fail(m);
  }

  int contentLen = http.getSize();
  Serial.printf("[ghupdate] content-length: %d\n", contentLen);
  if (contentLen <= 0) {
    http.end(); return fail("CDN content-length missing");
  }

  s_progress.totalBytes = (size_t)contentLen;
  s_progress.state      = Progress::Downloading;

  auto* netStream = http.getStreamPtr();

  // Heap-alloc the write buffer: a 4 KB stack array tips the 16 KB task stack
  // over during simultaneous TLS handshake + Update.write() call depth.
  uint8_t* buf = new (std::nothrow) uint8_t[4096];
  if (!buf) { http.end(); return fail("write buf OOM"); }

  // Download loop: poll with available() rather than blocking in readBytes().
  //
  // NetworkClientSecure::available() calls mbedtls_ssl_read(ctx, nullptr, 0),
  // which drains the TCP socket into mbedTLS's input state machine.
  // Blocking readBytes() holds Core 0 for the full stream timeout without
  // yielding, preventing lwIP from sending the TCP window update the CDN
  // needs before it transmits the next TLS record (stall at 32 KB boundary).
  //
  // Polling with 10 ms yields lets lwIP run between probes, keeping the
  // TCP receive window open so the CDN keeps streaming.
  static constexpr uint32_t kDownloadTimeoutMs = 120000;
  size_t written = 0;
  while (written < (size_t)contentLen) {
    uint32_t t0 = millis();
    while (netStream->available() <= 0) {
      if (!netStream->connected()) {
        char m[64]; snprintf(m, sizeof(m), "conn lost at %u/%u",
                              (unsigned)written, (unsigned)contentLen);
        delete[] buf; http.end(); return fail(m);
      }
      if (millis() - t0 > kDownloadTimeoutMs) {
        char m[64]; snprintf(m, sizeof(m), "stall at %u/%u free=%u",
                              (unsigned)written, (unsigned)contentLen,
                              ESP.getFreeHeap());
        delete[] buf; http.end(); return fail(m);
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    size_t remaining = (size_t)contentLen - written;
    int rd = netStream->read(buf, (int)min(remaining, (size_t)4096));
    if (rd <= 0) {
      char m[64]; snprintf(m, sizeof(m), "read %d at %u/%u",
                            rd, (unsigned)written, (unsigned)contentLen);
      delete[] buf; http.end(); return fail(m);
    }

    size_t wr = Update.write(buf, (size_t)rd);
    if (wr != (size_t)rd) {
      char m[64]; snprintf(m, sizeof(m), "write: %s", Update.errorString());
      delete[] buf; http.end(); return fail(m);
    }
    written += wr;
    s_progress.bytesWritten = written;
  }
  delete[] buf;
  http.end();

  Serial.printf("[ghupdate] downloaded %u bytes\n", (unsigned)written);
  // tls and http go out of scope here — TLS context freed

  if (!Update.end(true)) {
    char m[64]; snprintf(m, sizeof(m), "end: %s", Update.errorString());
    return fail(m);
  }

  return true;
}

// FreeRTOS task entry point — thin wrapper so C++ destructors run before vTaskDelete.
static void downloadTask(void* arg) {
  bool ok = downloadImpl(static_cast<const char*>(arg));
  if (ok) {
    Serial.println("[ghupdate] flash done — restarting");
    s_progress.state = Progress::Done;
    s_inFlight = false;
    vTaskDelay(pdMS_TO_TICKS(3000)); // let httpd serve the final "done" poll
    Net::restartToApply();
  }
  // On failure, downloadImpl already set state=Error and s_inFlight=false.
  vTaskDelete(nullptr);
}

// Called from the Arduino loop() (Core 1). Non-blocking: just spawns downloadTask.
void tick() {
  s_tickCount = s_tickCount + 1;
  if (!s_hasPending) return;
  s_hasPending = false;

  s_inFlight = true;
  s_progress = {};

  // Pin to Core 0 — same core as WiFi/lwIP/httpd; HTTPS works reliably there.
  // 16 KB stack: TLS buffers are heap-allocated; only function call depth needs stack.
  BaseType_t ok = xTaskCreatePinnedToCore(
      downloadTask, "ghota",
      16384,
      s_pendingBuf,
      5,
      nullptr,
      0
  );
  if (ok != pdPASS) {
    strlcpy(s_progress.errorMsg, "task create failed", sizeof(s_progress.errorMsg));
    s_progress.state = Progress::Error;
    s_inFlight = false;
  }
}

void registerRoutes(PsychicHttpServer& server) {
  server.on("/api/update/check", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    String latest = detectLatestTag();
    if (latest.isEmpty())
      return res->send(503, "application/json", "{\"error\":\"could not reach GitHub releases\"}");
    String current = "v" + String(SMOLBASE_FW_VERSION);
    bool upToDate  = (latest == current);
    String out = "{\"current\":\"" + current + "\",\"latest\":\"" + latest +
                 "\",\"upToDate\":" + (upToDate ? "true" : "false") + "}";
    return res->send(200, "application/json", out.c_str());
  });

  server.on("/api/update/ghprogress", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    static const char* const STATES[] = { "idle", "downloading", "done", "error" };
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"bytesWritten\":%u,\"totalBytes\":%u,\"error\":\"%s\","
             "\"ticks\":%u,\"hasPending\":%s}",
             STATES[(int)s_progress.state],
             (unsigned)s_progress.bytesWritten,
             (unsigned)s_progress.totalBytes,
             s_progress.errorMsg,
             (unsigned)s_tickCount,
             s_hasPending ? "true" : "false");
    return res->send(200, "application/json", buf);
  });

  // POST handler queues the download and returns immediately.
  // The actual work runs in downloadTask() pinned to Core 0.
  server.on("/api/update/github", HTTP_POST, [](PsychicRequest* req, PsychicResponse* res) {
    if (s_inFlight || s_hasPending)
      return res->send(409, "application/json", "{\"error\":\"update already in progress\"}");
    JsonDocument doc;
    if (deserializeJson(doc, req->body()) != DeserializationError::Ok)
      return res->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    String tag = doc["tag"] | String("");
    if (tag.isEmpty() || !tag.startsWith("v"))
      return res->send(400, "application/json", "{\"error\":\"missing or invalid tag\"}");

    Events::post(SysEvent::OtaStarting);
    strlcpy(s_pendingBuf, tag.c_str(), sizeof(s_pendingBuf));
    s_hasPending = true;
    return res->send(200, "application/json",
                     ("{\"ok\":true,\"tag\":\"" + tag + "\"}").c_str());
  });
}

} // namespace GhUpdate
