// GitHub release OTA: version check (HEAD redirect tag detection) and
// pull-and-flash (firmware then filesystem in one boot, single restart).
// Decisions: wayfinder map #106, tickets #107/#108/#109/#111.
//
// GET  /api/update/check   — HEAD redirect trick; returns {current,latest,upToDate}
// POST /api/update/github  — body {tag}; responds 200, then downloads+flashes+restarts
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

// GitHub repo and asset name prefixes — must match the CI release workflow.
static const char* const GH_REPO      = "sweetlilmre/smolbase";
static const char* const GH_FW_PREFIX = "smolbase-firmware";
static const char* const GH_FS_PREFIX = "smolbase-littlefs";

namespace GhUpdate {

// Same BundleClient pattern as WxHttp.cpp / CgmFetch.cpp.
// IDF 5.5.5 bundle covers both github.com (Sectigo) and
// objects.githubusercontent.com (DigiCert Global Root G2) — ticket #108.
class BundleClient : public NetworkClientSecure {
public:
  BundleClient() { attach_ssl_certificate_bundle(sslclient.get(), true); _use_ca_bundle = true; }
};

// Separate from Ota.cpp's s_inFlight — must not share that latch (#109 caution).
static bool s_inFlight = false;

// HEAD redirect trick: github.com/.../releases/latest returns 302 with the
// tag in the Location URL. Returns "v0.3.2" style tag or "" on failure — #107.
static String detectLatestTag() {
  BundleClient tls;
  HTTPClient http;
  http.setTimeout(10000);
  http.setConnectTimeout(10000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(tls, String("https://github.com/") + GH_REPO + "/releases/latest")) return "";
  int code = http.sendRequest("HEAD");
  String tag;
  if (code == 302) {
    String loc = http.getLocation(); // ".../releases/tag/v0.3.2"
    int idx = loc.lastIndexOf('/');
    if (idx >= 0) tag = loc.substring(idx + 1);
  }
  http.end();
  return tag;
}

// Stream a GitHub release asset into the given Update partition.
// Follows the github.com -> objects.githubusercontent.com redirect automatically.
// On false the Update state is clean; a second begin() is safe (#109).
static bool downloadAndFlash(const String& url, int partition) {
  Serial.printf("[ghupdate] downloading %s\n", url.c_str());
  BundleClient tls;
  HTTPClient http;
  http.setTimeout(60000);
  http.setConnectTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(tls, url)) {
    Serial.println("[ghupdate] http.begin failed");
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ghupdate] HTTP %d\n", code);
    http.end();
    return false;
  }
  int contentLen = http.getSize(); // -1 if unknown; Update checks partition fit at end
  if (!Update.begin(contentLen > 0 ? (size_t)contentLen : UPDATE_SIZE_UNKNOWN, partition)) {
    Serial.printf("[ghupdate] Update.begin failed: %s\n", Update.errorString());
    http.end();
    return false;
  }
  size_t written = Update.writeStream(*http.getStreamPtr());
  http.end();
  if (!Update.end(true)) {
    Serial.printf("[ghupdate] Update.end failed after %u bytes: %s\n",
                  (unsigned)written, Update.errorString());
    return false;
  }
  Serial.printf("[ghupdate] flashed %u bytes\n", (unsigned)written);
  return true;
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

  server.on("/api/update/github", HTTP_POST, [](PsychicRequest* req, PsychicResponse* res) {
    if (s_inFlight) {
      return res->send(409, "application/json",
                       "{\"error\":\"update already in progress\"}");
    }
    JsonDocument doc;
    if (deserializeJson(doc, req->body()) != DeserializationError::Ok) {
      return res->send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    }
    String tag = doc["tag"] | String("");
    if (tag.isEmpty() || !tag.startsWith("v")) {
      return res->send(400, "application/json",
                       "{\"error\":\"missing or invalid tag\"}");
    }

    s_inFlight = true;
    Events::post(SysEvent::OtaStarting); // apps: stop drawing, free buffers

    String base  = String("https://github.com/") + GH_REPO + "/releases/download/" + tag + "/";
    String fwUrl = base + GH_FW_PREFIX + "-" + tag + ".bin";
    String fsUrl = base + GH_FS_PREFIX + "-" + tag + ".bin";
    Serial.printf("[ghupdate] updating to %s\n", tag.c_str());

    // Send 200 before blocking on the download — browser calls handoff() and
    // shows the reload prompt. The response reaches the browser via lwIP while
    // the httpd task is busy with the downloads; no intermediate flush needed.
    // On firmware failure (no partition written yet), device stays on old image
    // and the browser reload finds it healthy.
    esp_err_t r = res->send(200, "application/json",
                            ("{\"ok\":true,\"tag\":\"" + tag + "\"}").c_str());

    if (!downloadAndFlash(fwUrl, U_FLASH)) {
      Serial.println("[ghupdate] firmware failed; staying on old image");
      s_inFlight = false;
      return r;
    }

    // Firmware staged in new OTA slot. Unmount LittleFS before rewriting the
    // data partition — the same guard Ota.cpp applies for ?target=fs uploads.
    LittleFS.end();
    if (!downloadAndFlash(fsUrl, U_SPIFFS)) {
      Serial.println("[ghupdate] fs failed after firmware staged; restarting");
      // Firmware is committed; FS state is unknown. Restart so the new firmware
      // boots — it serves the embedded RECOVERY_PAGE if the FS is torn.
      Net::restartToApply(); // no return
      return r;
    }

    Net::restartToApply(); // no return
    return r;
  });
}

} // namespace GhUpdate
