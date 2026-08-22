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
#include "Clock.h"
#include "ConfigStore.h"
#include "Events.h"
#include "Net.h"
#include "GhUpdate.h"
#include "Ota.h"
#include "Platform.h"
#include "Secrets.h"
#include "smolbase_config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <PsychicHttp.h>
#include <nvs_flash.h>

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

// Embedded recovery/update page (.rodata, like the fallback portal above).
// Always served at /recover, and for "/" in STA mode when the static index
// asset is missing — a wiped or failed filesystem never strands the user on
// curl. Uploads post to the same /api/update endpoint the settings page uses.
static const char RECOVERY_PAGE[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>smolbase recovery</title>
<style>body{font:16px sans-serif;margin:1.5rem;max-width:26rem}fieldset{border:1px solid #bbb;
margin:0 0 1rem;padding:.8rem}input[type=file]{width:100%;margin:.4rem 0}button{padding:.6rem 1rem;
font:inherit}progress{width:100%;display:none}.msg{color:#666;min-height:1.2em;font-size:.9em}</style>
</head><body><h2>smolbase recovery</h2><p class="msg" id="st">loading status…</p>
<fieldset><legend>Firmware</legend><input type="file" id="fw" accept=".bin">
<progress id="fwp" max="100"></progress><button id="fwb">Upload firmware</button>
<p class="msg" id="fwm"></p></fieldset>
<fieldset><legend>Filesystem (web assets)</legend>
<p class="msg">Replaces ALL web assets and settings.json — settings revert to defaults; WiFi credentials survive.</p>
<input type="file" id="fs" accept=".bin"><progress id="fsp" max="100"></progress>
<button id="fsb">Upload filesystem</button><p class="msg" id="fsm"></p></fieldset>
<script>
fetch("/api/status").then(function(r){return r.json()}).then(function(j){
document.getElementById("st").textContent=j.name+" · "+j.ip+" · fw "+j.fwVersion+
" · heap "+Math.round(j.heapFree/1024)+" kB free"}).catch(function(){})
function up(fid,bid,pid,mid,target){var b=document.getElementById(bid);
b.onclick=function(){var f=document.getElementById(fid).files[0],
m=document.getElementById(mid),p=document.getElementById(pid);
if(!f){m.textContent="Choose a .bin file first.";return}
b.disabled=true;p.style.display="block";p.value=0;
m.textContent="Uploading… don't power off the device.";
var fd=new FormData();fd.append("file",f,f.name);var x=new XMLHttpRequest();
x.open("POST","/api/update"+(target?"?target="+target:""));
x.upload.onprogress=function(e){if(e.lengthComputable)p.value=e.loaded/e.total*100};
x.onload=function(){b.disabled=false;p.style.display="none";var j={};
try{j=JSON.parse(x.responseText)}catch(e){}
m.textContent=x.status===200?"Update accepted — the device is restarting. Reload this page in ~15 s.":
"Failed ("+x.status+"): "+(j.error||"unexpected response")};
x.onerror=function(){b.disabled=false;p.style.display="none";
m.textContent="Connection lost — if the upload had just finished, the device may simply be restarting."};
x.send(fd)}}
up("fw","fwb","fwp","fwm","");up("fs","fsb","fsp","fsm","fs");
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
    doc["uptimeS"] = Platform::millis() / 1000;
    doc["heapFree"] = Platform::freeHeap();
    doc["timeSynced"] = Clock::isSynced(); // observability for SNTP stalls (#38)
    if (Net::isUp()) {
      doc["rssi"] = Net::rssi();
      doc["ssid"] = Net::ssid(); // which network we're on (#39)
    }
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
    Net::restartToApply(); // flushes the response, then Platform::restart(); no return
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

  // Re-provisioning without the sledgehammer (ticket #30): clears only the
  // stored WiFi credentials — settings.json survives — and restarts; the boot
  // state machine finds no creds and lands in AP/setup mode with the portal.
  httpServer.on("/api/wifi/forget", HTTP_POST, [](PsychicRequest*, PsychicResponse* res) {
    Net::clearCredentials();
    esp_err_t r = res->send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
    Net::restartToApply(); // no return
    return r;
  });

  // Secret store web surface (design: ticket #23; panel contract ADR 0003).
  // Write-only by construction: GET returns the descriptor map — labels,
  // hints, set-flags — NEVER values; POST accepts a flat multi-key object
  // where null deletes; nothing is ever echoed back.
  httpServer.on("/api/secrets", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    JsonDocument doc;
    Secrets::listJson(doc);
    return sendJson(res, 200, doc);
  });

  httpServer.on("/api/secrets", HTTP_POST, [](PsychicRequest* req, PsychicResponse* res) {
    JsonDocument doc;
    if (deserializeJson(doc, req->body()) != DeserializationError::Ok || !doc.is<JsonObject>()) {
      return res->send(400, "application/json", "{\"error\":\"expected a JSON object\"}");
    }
    for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
      bool ok = kv.value().isNull()
                    ? Secrets::clear(kv.key().c_str())
                    : Secrets::set(kv.key().c_str(), kv.value().as<std::string>());
      if (!ok) {
        return res->send(500, "application/json",
                         "{\"error\":\"secret store write failed (NVS full?)\"}");
      }
    }
    Events::post(SysEvent::SettingsChanged);
    return res->send(200, "application/json", "{\"ok\":true}");
  });

  // Factory reset is scorched-earth (charter amended by ticket #23): full NVS
  // erase — WiFi credentials, secrets, any consumer NVS data — plus
  // settings.json. RF calibration data goes too; it regenerates on the next
  // boot (one-time beat). WiFi is still running over the erased partition for
  // the ~200 ms until restart; any writes it attempts just fail silently.
  httpServer.on("/api/factory-reset", HTTP_POST, [](PsychicRequest*, PsychicResponse* res) {
    LittleFS.remove(SMOLBASE_SETTINGS_PATH);
    esp_err_t r = res->send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
    nvs_flash_deinit(); // best-effort; open handles just leak into the restart
    nvs_flash_erase();
    Net::restartToApply(); // no return
    return r;
  });

  // Dev loop (ticket #32): upload ONE file into LittleFS instead of reflashing
  // the whole image — POST /api/fs?path=/w/name.gz with a multipart file.
  // Streams via temp file + rename; parent dirs auto-created. Assets are
  // gzip-only, so the loop is: gzip the edited file, POST, refresh. This does
  // not replace fs-OTA, which stays the way to ship coherent images.
  {
    static File fsFile;
    static String fsPath;
    static bool fsFailed;
    auto* fsUp = new PsychicUploadHandler(); // lives for the server's lifetime
    fsUp->onUpload([](PsychicRequest* req, const String&, uint64_t index, uint8_t* data,
                      size_t len, bool last) -> esp_err_t {
      if (index == 0) {
        fsFailed = false;
        PsychicWebParameter* p = req->getParam("path");
        fsPath = p ? p->value() : String("");
        if (fsPath.length() < 2 || fsPath[0] != '/' || fsPath.indexOf("..") >= 0) {
          fsFailed = true;
          return ESP_OK; // drain; verdict in onRequest
        }
        for (int i = 1; (i = fsPath.indexOf('/', i)) > 0; ++i) {
          LittleFS.mkdir(fsPath.substring(0, i)); // no-op when it exists
        }
        fsFile = LittleFS.open("/.upload.tmp", "w");
        if (!fsFile) fsFailed = true;
      }
      if (fsFailed) return ESP_OK;
      if (len > 0 && fsFile.write(data, len) != len) {
        fsFile.close();
        fsFailed = true; // out of space, most likely
        return ESP_OK;
      }
      if (last) {
        fsFile.close();
        LittleFS.remove(fsPath);
        if (!LittleFS.rename("/.upload.tmp", fsPath)) fsFailed = true;
      }
      return ESP_OK;
    });
    fsUp->onRequest([](PsychicRequest*, PsychicResponse* res) {
      if (fsFailed) {
        return res->send(400, "application/json",
                         "{\"error\":\"fs upload failed (bad path, or out of space?)\"}");
      }
      return res->send(200, "application/json", "{\"ok\":true}");
    });
    httpServer.on("/api/fs", HTTP_POST, fsUp);
  }

  // Recovery page: registered as a real endpoint so it works in every mode
  // regardless of filesystem state. The uploads it drives are Ota's routes.
  httpServer.on("/recover", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    return res->send(200, "text/html", RECOVERY_PAGE);
  });

  // --- 2. OTA slots ---
  Ota::registerRoutes(httpServer);
  GhUpdate::registerRoutes(httpServer);

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
    } else if (req->uri() == "/" || req->uri() == "/settings.html") {
      // STA mode with the asset missing (wiped/failed filesystem): serve the
      // embedded recovery page so the device is never browser-dead — also for
      // bookmarked settings.html. A *present but broken* asset still serves
      // normally; /recover (a firmware route, matched before static files) is
      // the escape hatch for that case.
      return res->send(200, "text/html", RECOVERY_PAGE);
    }
    return res->send(404, "text/plain", "Not found");
  });
}

} // namespace Web
