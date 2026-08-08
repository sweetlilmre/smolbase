// See Web.h for the structural registration order. Verified against the
// pinned PsychicHttp 3.1.2 source (.pio/libdeps/*/PsychicHttp/src):
//  - Static serving auto-falls-back to "<name>.gz" with Content-Encoding: gzip
//    (PsychicStaticFileHander.cpp — upstream filename typo — _fileExists()).
//  - ON_AP_FILTER / ON_STA_FILTER are free functions keyed off the netif that
//    carries the request, so they stay correct in AP_STA (scan) mode.
//  - server.rewrite(from, to)->setFilter(...) rewrites the URI before endpoint
//    matching — how "/" lands on the portal in AP mode without shadowing the
//    STA-mode index.html.
//  - onNotFound() installs the default endpoint, consulted only after every
//    endpoint and handler has passed — the captive catch-all slot.
//  - PsychicResponse::redirect() defaults to 301 unless a code was set first;
//    captive probes want 302, hence the explicit setCode.
#include "Web.h"
#include "App.h"
#include "Net.h"
#include "Ota.h"
#include "smolbase_config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <PsychicHttp.h>

namespace Web {

static PsychicHttpServer httpServer;

PsychicHttpServer& server() { return httpServer; }

static esp_err_t sendJson(PsychicResponse* res, int code, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  return res->send(code, "application/json", out.c_str());
}

void begin(App& app) {
  httpServer.config.core_id = 0; // network core; consumer code stays on core 1 (ADR 0001)
  // Phones fire many parallel connectivity probes at a captive portal; LRU
  // purging stops a burst from exhausting the socket pool (research #4).
  httpServer.config.lru_purge_enable = true;
  httpServer.setPort(80);
  httpServer.start();

  // --- 1. system API routes ---
  httpServer.on("/api/status", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    JsonDocument doc;
    doc["name"] = Net::deviceName();
    doc["ip"] = Net::ip().toString();
    doc["apMode"] = Net::inApMode();
    doc["fwVersion"] = SMOLBASE_FW_VERSION;
    doc["uptimeS"] = millis() / 1000;
    doc["heapFree"] = ESP.getFreeHeap();
    if (Net::isUp()) doc["rssi"] = Net::rssi();
    return sendJson(res, 200, doc);
  });

  // Poll target for the portal/settings UI. A plain GET returns the current
  // results (Net auto-starts the first scan and self-heals failed ones);
  // ?refresh=1 kicks a fresh scan (a no-op while one is already running).
  // Kicking unconditionally would restart the scan on every poll and the
  // client would never observe "done".
  httpServer.on("/api/wifi/scan", HTTP_GET, [](PsychicRequest* req, PsychicResponse* res) {
    if (req->hasParam("refresh")) Net::scanNetworks();
    JsonDocument doc;
    Net::scanResultsJson(doc);
    return sendJson(res, 200, doc);
  });

  // Provisioning: {"ssid":"...","pass":"..."} -> save to NVS, ack, restart.
  httpServer.on("/api/wifi", HTTP_POST, [](PsychicRequest* req, PsychicResponse* res) {
    JsonDocument doc;
    if (deserializeJson(doc, req->body()) != DeserializationError::Ok ||
        doc["ssid"].as<String>().isEmpty()) {
      return res->send(400, "application/json", "{\"error\":\"expected {ssid,pass}\"}");
    }
    Net::saveCredentials(doc["ssid"].as<String>(), doc["pass"] | String(""));
    esp_err_t r = res->send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
    Net::restartToApply(); // flushes the response, then ESP.restart(); no return
    return r;
  });

  httpServer.on("/api/factory-reset", HTTP_POST, [](PsychicRequest*, PsychicResponse* res) {
    Net::clearCredentials();
    LittleFS.remove(SMOLBASE_SETTINGS_PATH);
    esp_err_t r = res->send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
    Net::restartToApply(); // no return
    return r;
  });

  // --- 2. OTA slot (real upload handler is ticket #18) ---
  Ota::registerRoutes(httpServer);

  // --- 3. consumer routes ---
  app.registerRoutes(httpServer);

  // --- 4. static assets (gzip-only files; PsychicHttp auto-serves name.gz) ---
  httpServer.serveStatic("/", LittleFS, SMOLBASE_WWW_DIR "/")
      ->setDefaultFile("index.html")
      ->setCacheControl("max-age=300");
  // AP mode: "/" lands on the provisioning portal (asset ships with ticket
  // #13). Rewritten pre-dispatch so STA mode still gets index.html above.
  httpServer.rewrite("/", "/portal.html")->setFilter(ON_AP_FILTER);

  // --- 5. captive-portal catch-all (must stay last) ---
  // Requests for hosts other than our AP IP (the OS connectivity-probe dance:
  // generate_204, hotspot-detect.html, ...) are bounced to the portal root.
  // Requests already addressed to us fall through to a plain 404 — never
  // redirect those, or a missing asset would loop the browser forever.
  httpServer.onNotFound([](PsychicRequest* req, PsychicResponse* res) -> esp_err_t {
    if (Net::inApMode()) {
      String self = Net::ip().toString();
      if (req->host() != self) {
        res->setCode(302);
        return res->redirect((String("http://") + self + "/").c_str());
      }
    }
    return res->send(404, "text/plain", "Not found");
  });
}

} // namespace Web
