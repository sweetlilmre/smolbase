// GitHub release OTA: version check and firmware-only flash from public releases.
// Filesystem is managed separately (see wayfinder map #106).
// Decisions: wayfinder map #106, tickets #107/#108/#109.
//
// GET  /api/update/check      — HEAD redirect trick; returns {current,latest,upToDate}
// GET  /api/update/ghprogress — live state for the settings UI polling loop
// POST /api/update/github     — body {tag}; launches background flash task, responds 200
#include "GhUpdate.h"
#include "Events.h"
#include "Net.h"
#include "smolbase_config.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <PsychicHttp.h>
#include <Update.h>
#include <esp_crt_bundle.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// GitHub repo and asset prefix — must match the CI release workflow.
static const char* const GH_REPO      = "sweetlilmre/smolbase";
static const char* const GH_FW_PREFIX = "smolbase-firmware";

namespace GhUpdate {

// Same BundleClient pattern as WxHttp.cpp — IDF 5.5.5 bundle covers both
// github.com (Sectigo) and objects.githubusercontent.com (DigiCert G2).
class BundleClient : public NetworkClientSecure {
public:
  BundleClient() { attach_ssl_certificate_bundle(sslclient.get(), true); _use_ca_bundle = true; }
};

// Progress updated by the flash task, polled by the progress endpoint.
// bytesWritten and state are volatile so the compiler doesn't cache them
// across the task boundary; the ESP32 cache is coherent so no barrier needed.
struct Progress {
  enum State : uint8_t { Idle, Downloading, Done, Error };
  volatile State  state        = Idle;
  volatile size_t bytesWritten = 0;
  size_t          totalBytes   = 0;
  char            errorMsg[80] = {};
};
static Progress s_progress;
static bool s_inFlight = false;

// GitHub API: fetch /releases/latest and extract tag_name.
// ArduinoJson filter skips everything except tag_name so heap cost is minimal.
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

// FreeRTOS task: download firmware, flash, restart.
// Runs on the network core so it doesn't starve the httpd task and the
// httpd task can continue serving /api/update/ghprogress polls.
static void flashTask(void* arg) {
  char* tagBuf = static_cast<char*>(arg);
  String tag(tagBuf);
  free(tagBuf);

  String url = String("https://github.com/") + GH_REPO + "/releases/download/" + tag +
               "/" + GH_FW_PREFIX + "-" + tag + ".bin";
  Serial.printf("[ghupdate] downloading %s\n", url.c_str());

  auto fail = [](const char* fmt, ...) {
    char msg[80];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
    Serial.printf("[ghupdate] failed: %s\n", msg);
    strlcpy(s_progress.errorMsg, msg, sizeof(s_progress.errorMsg));
    s_progress.state = Progress::Error;
    s_inFlight = false;
  };

  BundleClient tls;
  HTTPClient   http;
  http.setTimeout(120000);
  http.setConnectTimeout(15000);
  // STRICT_FOLLOW_REDIRECTS handles github.com → objects.githubusercontent.com;
  // the redirect preserves GET and the BundleClient reconnects with the same
  // cert bundle (confirmed via probe on 2026-08-20).
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(tls, url)) {
    fail("http.begin failed"); http.end(); vTaskDelete(nullptr); return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    fail("HTTP %d", code); http.end(); vTaskDelete(nullptr); return;
  }

  int contentLen = http.getSize();
  s_progress.totalBytes = contentLen > 0 ? (size_t)contentLen : 0;

  if (!Update.begin(contentLen > 0 ? (size_t)contentLen : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    fail("Update.begin: %s", Update.errorString()); http.end(); vTaskDelete(nullptr); return;
  }

  s_progress.state = Progress::Downloading;

  // Chunk loop: keeps s_progress.bytesWritten live for the polling endpoint.
  NetworkClient* stream = http.getStreamPtr();
  int remaining = contentLen;
  size_t written = 0;
  bool ok = true;
  uint8_t buf[1024];

  while (http.connected() && (remaining < 0 || remaining > 0)) {
    int avail = stream->available();
    if (avail > 0) {
      int toRead = min(avail, (int)sizeof(buf));
      if (remaining > 0 && toRead > remaining) toRead = remaining;
      int rd = stream->readBytes(buf, toRead);
      if (rd <= 0) break;
      size_t wr = Update.write(buf, (size_t)rd);
      written += wr;
      s_progress.bytesWritten = written;
      if (wr != (size_t)rd) { ok = false; break; }
      if (remaining > 0) remaining -= rd;
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  http.end();

  if (!ok) {
    Update.abort();
    fail("write error after %u bytes", (unsigned)written);
    vTaskDelete(nullptr);
    return;
  }
  if (!Update.end(true)) {
    fail("Update.end: %s", Update.errorString());
    vTaskDelete(nullptr);
    return;
  }

  Serial.printf("[ghupdate] firmware flashed: %u bytes\n", (unsigned)written);
  s_progress.state = Progress::Done;
  // 1.5 s pause so the browser can poll Done before the device disappears.
  vTaskDelay(pdMS_TO_TICKS(1500));
  Net::restartToApply(); // no return
  vTaskDelete(nullptr);
}

void registerRoutes(PsychicHttpServer& server) {
  server.on("/api/update/check", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    String latest = detectLatestTag();
    if (latest.isEmpty()) {
      return res->send(503, "application/json",
                       "{\"error\":\"could not reach GitHub releases\"}");
    }
    String current = "v" + String(SMOLBASE_FW_VERSION);
    bool upToDate  = (latest == current);
    String out = "{\"current\":\"" + current + "\",\"latest\":\"" + latest +
                 "\",\"upToDate\":" + (upToDate ? "true" : "false") + "}";
    return res->send(200, "application/json", out.c_str());
  });

  server.on("/api/update/ghprogress", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    static const char* const STATES[] = { "idle", "downloading", "done", "error" };
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"bytesWritten\":%u,\"totalBytes\":%u,\"error\":\"%s\"}",
             STATES[(int)s_progress.state],
             (unsigned)s_progress.bytesWritten,
             (unsigned)s_progress.totalBytes,
             s_progress.errorMsg);
    return res->send(200, "application/json", buf);
  });

  server.on("/api/update/github", HTTP_POST, [](PsychicRequest* req, PsychicResponse* res) {
    if (s_inFlight) {
      return res->send(409, "application/json", "{\"error\":\"update already in progress\"}");
    }
    JsonDocument doc;
    if (deserializeJson(doc, req->body()) != DeserializationError::Ok) {
      return res->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    }
    String tag = doc["tag"] | String("");
    if (tag.isEmpty() || !tag.startsWith("v")) {
      return res->send(400, "application/json", "{\"error\":\"missing or invalid tag\"}");
    }

    s_inFlight = true;
    s_progress = {};
    Events::post(SysEvent::OtaStarting);

    char* tagBuf = strdup(tag.c_str());
    if (!tagBuf || xTaskCreate(flashTask, "ghflash", 16384, tagBuf, 5, nullptr) != pdPASS) {
      free(tagBuf);
      s_inFlight = false;
      return res->send(503, "application/json", "{\"error\":\"could not start flash task\"}");
    }
    return res->send(200, "application/json",
                     ("{\"ok\":true,\"tag\":\"" + tag + "\"}").c_str());
  });
}

} // namespace GhUpdate
