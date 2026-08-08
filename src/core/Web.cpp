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
#include "ConfigStore.h"
#include "Net.h"
#include "Ota.h"
#include "smolbase_config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <PsychicHttp.h>

namespace Web {

// Minimal provisioning page compiled into the firmware (.rodata). Served from
// the captive catch-all ONLY when the real /w/portal.html asset is absent —
// i.e. a firmware-only OTA landed on a filesystem without smolbase assets.
// Kept deliberately tiny; the full-featured page lives in html/portal.html.
static const char FALLBACK_PORTAL[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>smolbase setup</title>
<style>body{font:16px sans-serif;margin:1.5rem;max-width:26rem}button{display:block;width:100%;
margin:.4rem 0;padding:.7rem;font:inherit;text-align:left}#msg{color:#666}</style></head><body>
<h2>smolbase Wi-Fi setup</h2><p>(fallback page — upload the filesystem image after joining)</p>
<div id="nets"></div><p id="msg">scanning…</p>
<script>
function esc(s){return s.replace(/[&<>"']/g,function(c){return "&#"+c.charCodeAt(0)+";"})}
function join(ssid,sec){var pass=sec?prompt("Password for "+ssid):"";if(pass===null)return;
document.getElementById("msg").textContent="joining "+ssid+"…";
fetch("/api/wifi",{method:"POST",headers:{"Content-Type":"application/json"},
body:JSON.stringify({ssid:ssid,pass:pass||""})}).then(function(r){return r.json()}).then(function(j){
document.getElementById("msg").textContent=j.restarting?
"Device is restarting to join "+ssid+". Reconnect your phone to that network.":JSON.stringify(j)})
.catch(function(){document.getElementById("msg").textContent="Request failed - if the device rebooted, it may have worked."})}
function poll(){fetch("/api/wifi/scan").then(function(r){return r.json()}).then(function(j){
if(j.status!=="done"){setTimeout(poll,2500);return}
var d=document.getElementById("nets");d.innerHTML="";
j.networks.forEach(function(n){var b=document.createElement("button");
b.innerHTML=esc(n.ssid)+(n.secure?" &#128274;":"")+" ("+n.rssi+" dBm)";
b.onclick=function(){join(n.ssid,n.secure)};d.appendChild(b)});
document.getElementById("msg").textContent=j.networks.length+" networks";}).catch(function(){setTimeout(poll,2500)})}
poll();
</script></body></html>)HTML";

static PsychicHttpServer httpServer;

PsychicHttpServer& server() { return httpServer; }

static esp_err_t sendJson(PsychicResponse* res, int code, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  return res->send(code, "application/json", out.c_str());
}

void start() {
  // Idempotent (start() returns ESP_OK when already running). Called from the
  // NetworkUp/ApModeEntered handlers — the moments a netif provably has an IP.
  esp_err_t err = httpServer.start();
  if (err != ESP_OK) Serial.printf("[web] server start failed: %d\n", (int)err);
}

void begin(App& app) {
  httpServer.config.core_id = 0; // network core; consumer code stays on core 1 (ADR 0001)
  // Phones fire many parallel connectivity probes at a captive portal; LRU
  // purging stops a burst from exhausting the socket pool (research #4).
  httpServer.config.lru_purge_enable = true;
  httpServer.setPort(80);
  // NOTE: no start() here — see Web.h. Routes register fine pre-start.

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
    if (!Net::saveCredentials(doc["ssid"].as<String>(), doc["pass"] | String(""))) {
      return res->send(500, "application/json",
                       "{\"error\":\"failed to store credentials (NVS full?)\"}");
    }
    esp_err_t r = res->send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
    Net::restartToApply(); // flushes the response, then ESP.restart(); no return
    return r;
  });

  // Settings contract (ticket #14): the schema registry is the single source of
  // truth — GET returns schema + current values, POST applies a flat value map.
  // The served settings page is a pure static asset built against this contract.
  httpServer.on("/api/settings", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    JsonDocument doc;
    ConfigStore::schemaToJson(doc);
    return sendJson(res, 200, doc);
  });

  httpServer.on("/api/settings", HTTP_POST, [](PsychicRequest* req, PsychicResponse* res) {
    JsonDocument doc;
    if (deserializeJson(doc, req->body()) != DeserializationError::Ok || !doc.is<JsonObject>()) {
      return res->send(400, "application/json", "{\"error\":\"expected a JSON object\"}");
    }
    bool changed = ConfigStore::applyJson(doc.as<JsonObjectConst>());
    if (changed && !ConfigStore::save()) {
      return res->send(500, "application/json", "{\"error\":\"failed to persist settings\"}");
    }
    JsonDocument out;
    out["ok"] = true;
    out["changed"] = changed;
    return sendJson(res, 200, out);
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
  // Requests already addressed to us fall through — never redirect those, or a
  // missing asset would loop the browser forever. If the portal PAGE itself is
  // missing (first flash arrives by OTA and the old filesystem has no smolbase
  // assets), serve the embedded fallback so provisioning is never UI-dead.
  httpServer.onNotFound([](PsychicRequest* req, PsychicResponse* res) -> esp_err_t {
    if (Net::inApMode()) {
      String self = Net::ip().toString();
      const String& host = req->host();
      bool selfAddressed = host == self || host.startsWith(self + ":");
      if (!selfAddressed) {
        res->setCode(302);
        return res->redirect((String("http://") + self + "/").c_str());
      }
      if (req->uri() == "/" || req->uri() == "/portal.html") {
        return res->send(200, "text/html", FALLBACK_PORTAL);
      }
    }
    return res->send(404, "text/plain", "Not found");
  });
}

} // namespace Web
