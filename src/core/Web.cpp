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

void begin(App& app) {
  httpServer.config.core_id = 0; // network core; consumer code stays on core 1 (ADR 0001)
  httpServer.setPort(80);
  httpServer.start();

  // --- system API routes first ---
  httpServer.on("/api/status", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    JsonDocument doc;
    doc["name"] = Net::deviceName();
    doc["ip"] = Net::ip().toString();
    doc["apMode"] = Net::inApMode();
    doc["heapFree"] = ESP.getFreeHeap();
    String out;
    serializeJson(doc, out);
    return res->send(200, "application/json", out.c_str());
  });

  Ota::registerRoutes(httpServer);

  // --- consumer routes second ---
  app.registerRoutes(httpServer);

  // --- static assets LAST (gzip-only files; PsychicHttp auto-serves name.gz) ---
  httpServer.serveStatic("/", LittleFS, SMOLBASE_WWW_DIR "/")->setDefaultFile("index.html");
}

} // namespace Web
