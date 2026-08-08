// OTA via PsychicUploadHandler streaming into Update.h (research #4 pattern).
// Multipart bodies don't carry a total size up front, hence
// Update.begin(UPDATE_SIZE_UNKNOWN); partition fit is checked at Update.end().
// The upload callback must keep returning ESP_OK even after an Update error —
// PsychicUploadHandler only runs onRequest when every chunk callback returned
// ESP_OK, and onRequest is where we send the JSON verdict. So on error we
// remember it, drain the remaining chunks, and answer 400 from onRequest.
// Update.abort() overwrites the real error with "Aborted", so errorString()
// is captured first. Restart happens via Net::restartToApply() AFTER the
// 200 response is sent (never mid-handler, per the arduino_ota example).
#include "Ota.h"
#include "Events.h"
#include "Net.h"
#include <PsychicHttp.h>
#include <Update.h>

namespace Ota {

// One request at a time is guaranteed by the single httpd task (no
// ENABLE_ASYNC), so plain statics carry per-request state from onUpload to
// onRequest. s_inFlight additionally rejects a new upload racing the
// restart window of a completed one.
enum class Outcome : uint8_t { None, Ok, Failed, Rejected };
static Outcome s_outcome = Outcome::None;
static bool s_inFlight = false;
static size_t s_progress = 0; // bytes written so far
static String s_error;

static void fail(bool abortUpdate) {
  s_error = Update.errorString(); // before abort() clobbers it
  Update.printError(Serial);
  if (abortUpdate) Update.abort();
  s_outcome = Outcome::Failed;
  s_inFlight = false;
}

static esp_err_t onUploadChunk(PsychicRequest* req, const String& filename,
                               uint64_t index, uint8_t* data, size_t len, bool last) {
  if (index == 0) {
    if (s_inFlight) { // another update is active (or finished, restart pending)
      s_outcome = Outcome::Rejected;
      return ESP_OK; // drain; verdict goes out in onRequest
    }
    s_outcome = Outcome::None;
    s_inFlight = true;
    s_progress = 0;
    s_error = "";
    Events::post(SysEvent::OtaStarting); // apps: stop drawing/allocating

    // ?target=fw (default) -> app partition; ?target=fs -> data partition.
    // U_SPIFFS is arduino-esp32's constant for the data partition; it is
    // what updates LittleFS too. Works as a form field only if it precedes
    // the file part, so the query string is the documented spelling.
    PsychicWebParameter* p = req->getParam("target");
    const bool fs = (p != nullptr && p->value() == "fs");
    Serial.printf("[ota] %s update starting (%s)\n",
                  fs ? "filesystem" : "firmware", filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, fs ? U_SPIFFS : U_FLASH)) {
      fail(false);
      return ESP_OK;
    }
  }

  if (s_outcome != Outcome::None) return ESP_OK; // already decided; drain

  if (len > 0 && Update.write(data, len) != len) {
    fail(true);
    return ESP_OK;
  }
  s_progress += len;

  if (last) {
    if (Update.end(true)) {
      Serial.printf("[ota] update complete: %u bytes\n", (unsigned)s_progress);
      s_outcome = Outcome::Ok; // s_inFlight stays set: restart is imminent
    } else {
      fail(false);
    }
  }
  return ESP_OK;
}

static esp_err_t onUploadDone(PsychicRequest*, PsychicResponse* res) {
  switch (s_outcome) {
    case Outcome::Ok: {
      esp_err_t r = res->send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
      Net::restartToApply(); // flushes the response, then ESP.restart(); no return
      return r;
    }
    case Outcome::Rejected:
      s_outcome = Outcome::None;
      return res->send(409, "application/json", "{\"error\":\"update already in progress\"}");
    case Outcome::Failed: {
      s_outcome = Outcome::None;
      String out = "{\"error\":\"";
      out += s_error;
      out += "\"}";
      return res->send(400, "application/json", out.c_str());
    }
    case Outcome::None: // body carried no file part at all
    default:
      return res->send(400, "application/json", "{\"error\":\"no file uploaded\"}");
  }
}

void registerRoutes(PsychicHttpServer& server) {
  // The library default MAX_UPLOAD_SIZE (2 MB) is smaller than the 2.17 MB
  // app partition; a near-full firmware image would be refused with a 400
  // before our handler ever ran. Update.end() still enforces the real
  // partition fit, so a generous cap is safe.
  server.maxUploadSize = 4 * 1024 * 1024;

  auto* handler = new PsychicUploadHandler(); // lives for the server's lifetime
  handler->onUpload(onUploadChunk);
  handler->onRequest(onUploadDone);
  server.on("/api/update", HTTP_POST, handler);

  server.on("/api/update/status", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"inProgress\":%s,\"progress\":%u}",
             s_inFlight ? "true" : "false", (unsigned)s_progress);
    return res->send(200, "application/json", buf);
  });
}

} // namespace Ota
