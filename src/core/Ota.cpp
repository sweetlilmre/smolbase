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
#include <LittleFS.h>
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
static bool s_isFs = false;   // current upload targets the data partition
static int s_socket = -1;     // socket carrying the in-flight upload
static size_t s_progress = 0; // bytes written so far
static String s_error;

static void fail(bool abortUpdate) {
  s_error = Update.errorString(); // before abort() clobbers it
  Update.printError(Serial);
  if (abortUpdate) Update.abort();
  s_outcome = Outcome::Failed;
  // fs-target failures keep the latch: LittleFS is unmounted, so the device
  // must reboot — either onUploadDone's Failed branch or, if the client
  // vanished first, the onClose backstop delivers that reboot.
  if (!s_isFs) s_inFlight = false;
}

static esp_err_t onUploadChunk(PsychicRequest* req, const String& filename,
                               uint64_t index, uint8_t* data, size_t len, bool last) {
  if (index == 0) {
    if (s_inFlight) {
      // Another update is active, or a SECOND file part arrived in the same
      // multipart body after a successful end() — the boot partition is
      // already switched in that case, so the Ok verdict must survive.
      if (s_outcome != Outcome::Ok) s_outcome = Outcome::Rejected;
      return ESP_OK; // drain; verdict goes out in onRequest
    }
    s_outcome = Outcome::None;
    s_inFlight = true;
    s_socket = req->client() ? req->client()->socket() : -1;
    s_progress = 0;
    s_error = "";
    Events::post(SysEvent::OtaStarting); // apps: stop drawing/allocating

    // ?target=fw (default) -> app partition; ?target=fs -> data partition.
    // U_SPIFFS is arduino-esp32's constant for the data partition; it is
    // what updates LittleFS too. Works as a form field only if it precedes
    // the file part, so the query string is the documented spelling.
    PsychicWebParameter* p = req->getParam("target");
    s_isFs = (p != nullptr && p->value() == "fs");
    Serial.printf("[ota] %s update starting (%s)\n",
                  s_isFs ? "filesystem" : "firmware", filename.c_str());

    if (s_isFs) {
      // Guard against flashing a non-filesystem image over the data partition
      // (Update.h verifies NOTHING for U_SPIFFS — a firmware.bin here would
      // destroy the filesystem). A littlefs image carries "littlefs" at
      // offset 8; require enough bytes in the first chunk to check.
      if (len < 16 || memcmp(data + 8, "littlefs", 8) != 0) {
        s_error = "not a littlefs image (magic missing at offset 8)";
        s_outcome = Outcome::Failed;
        s_inFlight = false;
        return ESP_OK;
      }
      // The partition is about to be rewritten under a mounted filesystem;
      // unmount first so nothing writes through stale metadata during or
      // after the flash (SmolTV-Pro proved this pattern). From here on the
      // device MUST restart — with or without a successful update.
      LittleFS.end();
    }

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, s_isFs ? U_SPIFFS : U_FLASH)) {
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
      out += s_isFs ? "\",\"restarting\":true}" : "\"}";
      esp_err_t r = res->send(400, "application/json", out.c_str());
      // A failed fs update leaves LittleFS unmounted (and possibly a torn
      // image); rebooting re-mounts the old image or formats — either way the
      // device stays serviceable. Firmware failures need no restart.
      if (s_isFs) Net::restartToApply(); // no return
      return r;
    }
    case Outcome::None: // no file part, or a multipart parse error mid-file
    default:
      // A malformed body can abort the part callbacks after Update.begin()
      // succeeded (the processor drains bytes and still reports ESP_OK) —
      // without this cleanup the in-flight latch would block all future
      // updates until a power cycle.
      if (s_inFlight) {
        if (Update.isRunning()) Update.abort();
        s_inFlight = false;
        if (s_isFs) { // filesystem may be half-written; a reboot re-mounts or formats
          esp_err_t r = res->send(400, "application/json",
                                  "{\"error\":\"upload incomplete; restarting\"}");
          Net::restartToApply(); // no return
          return r;
        }
      }
      return res->send(400, "application/json", "{\"error\":\"no file uploaded\"}");
  }
}

// The upload handler's onRequest does NOT run when a chunk callback or the
// multipart processor returns an error (the library sends its own 500) — and
// it also doesn't run if the client vanishes mid-upload. The server-level
// close callback is the backstop that keeps the update channel usable: reset
// the latch, abort a half-written Update, and honor an already-committed one
// (the boot partition is switched the moment end() succeeds, so restarting is
// the honest move). Matched against the uploading connection's socket so a
// stray probe disconnect can't abort a healthy in-flight update. NOTE: this
// claims the server's single onClose slot; consumers needing their own close
// hook must chain through it.
static void onSocketClose(PsychicClient* client) {
  if (!s_inFlight || client == nullptr || client->socket() != s_socket) return;
  if (s_outcome == Outcome::Ok) {
    Serial.println("[ota] client gone after successful update; restarting");
    Net::restartToApply(); // no return
  }
  Serial.println("[ota] upload aborted (client disconnect / parse error)");
  if (Update.isRunning()) Update.abort();
  s_inFlight = false;
  s_outcome = Outcome::None;
  if (s_isFs) Net::restartToApply(); // filesystem is unmounted/possibly torn; reboot
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
  server.onClose(onSocketClose); // disconnect backstop — see comment above

  server.on("/api/update/status", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"inProgress\":%s,\"progress\":%u}",
             s_inFlight ? "true" : "false", (unsigned)s_progress);
    return res->send(200, "application/json", buf);
  });
}

} // namespace Ota
