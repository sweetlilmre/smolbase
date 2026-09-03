// OTA via PsychicUploadHandler streaming into esp_ota_ops / esp_partition.
//
// Two targets, and they are NOT symmetric:
//   firmware -> esp_ota_ops (begin/write/end/set_boot_partition)
//   fs       -> raw esp_partition writes, because esp_ota has no
//               data-partition equivalent. Arduino's U_SPIFFS was
//               arduino-esp32's own invention and, as the guard below notes,
//               verified nothing about what it was writing.
//
// Erase strategy differs for a real reason. esp_ota_write does not erase;
// esp_ota_begin does it up front, and with OTA_SIZE_UNKNOWN that means the
// whole 2.125 MB slot (~2-3 s of blocking flash erase on the httpd task).
// Content-Length gives a much tighter bound, so it is used when present and
// OTA_SIZE_UNKNOWN is only the fallback. The fs path erases lazily, one sector
// ahead of the write front: erasing its 3.8 MB partition up front would block
// for ~20 s and trip the task watchdog.
// The upload callback must keep returning ESP_OK even after an Update error —
// PsychicUploadHandler only runs onRequest when every chunk callback returned
// ESP_OK, and onRequest is where we send the JSON verdict. So on error we
// remember it, drain the remaining chunks, and answer 400 from onRequest.
// Each writer call records its own esp_err_to_name() on failure, so the real
// cause survives the abort that follows (Arduino's Update.abort() used to
// overwrite it with "Aborted"). Restart happens via Net::restartToApply()
// AFTER the 200 response is sent, never mid-handler.
#include "Ota.h"
#include "AssetUpdate.h"
#include "Events.h"
#include "Net.h"
#include "Platform.h"
#include <PsychicHttp.h>
#include <esp_littlefs.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <spi_flash_mmap.h>
#include <cstring>
#include <cstdio>

// Boot-loop guard (ticket #76): the arduino core's initArduino() normally
// confirms a PENDING_VERIFY image immediately at boot. Returning true here
// defers that — tickRollbackGuard() below confirms only after healthy
// uptime, so a boot-crashing image gets rolled back by the bootloader
// instead of boot-looping an OTA-only device into a bricked state.
// C linkage: the weak symbol lives in esp32-hal-misc.c.
extern "C" bool verifyRollbackLater() { return true; }

namespace Ota {

// Minimum uptime before a fresh image may be confirmed. 30 s covers the boot
// path (app setup, first paints, WiFi join) with margin, while staying short
// enough that a same-session follow-up OTA is never blocked on it. This is a
// FLOOR, not the whole test — see tickRollbackGuard, which also requires the
// device to be reachable.
static constexpr uint32_t CONFIRM_UPTIME_MS = 30000;

void tickRollbackGuard() {
  static bool done = false;
  if (done) return;
  if (Platform::millis() < CONFIRM_UPTIME_MS) return;

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  const bool pending = esp_ota_get_state_partition(running, &state) == ESP_OK &&
                       state == ESP_OTA_IMG_PENDING_VERIFY;

  if (pending) {
    // Uptime alone is NOT evidence of health on a device with no serial
    // flasher. An image that boots, paints, and then never reaches the network
    // is unreachable — and confirming it cancels the rollback that was the only
    // way back. Require that the device is actually REACHABLE: either the STA
    // link is up, or AP mode is raised (a device waiting to be provisioned is
    // working correctly, and its portal is reachable).
    //
    // Deliberately keeps retrying instead of deciding once. A device that is
    // mid-reconnect at the 30 s mark is healthy, and a single-shot check would
    // strand it on a false rollback. If neither state is ever reached the image
    // simply stays PENDING_VERIFY and the next reset rolls it back, which is
    // the correct outcome for a firmware that cannot be talked to.
    if (!Net::isUp() && !Net::inApMode()) return;
    esp_ota_mark_app_valid_cancel_rollback();
    printf("[ota] image confirmed - device reachable, rollback armed off\n");
  }

  done = true;
  // The image is stable: an asset backup for a *different* version can never
  // be needed again (only a rollback would want it, and rollback is off).
  // Gated behind the reachability check above for the same reason — while a
  // rollback is still possible, the previous version's assets must survive.
  AssetUpdate::onImageConfirmed();
}

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
static std::string s_error;

// ---- write target ---------------------------------------------------------
// One shape over two very different mechanisms; see the file header.
static esp_ota_handle_t s_ota = 0;
static const esp_partition_t* s_part = nullptr;
static bool s_running = false;
static size_t s_fsOffset = 0; // fs path: write front
static size_t s_fsErased = 0; // fs path: erased up to here (sector-aligned)

static bool writerBegin(size_t sizeHint) {
  s_running = false;
  s_fsOffset = 0;
  s_fsErased = 0;
  if (s_isFs) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      "spiffs");
    if (!s_part) {
      s_error = "data partition 'spiffs' not found";
      return false;
    }
  } else {
    s_part = esp_ota_get_next_update_partition(nullptr);
    if (!s_part) {
      s_error = "no free OTA slot";
      return false;
    }
    // esp_ota_begin REFUSES to start while the running image is
    // PENDING_VERIFY (ESP_ERR_OTA_ROLLBACK_INVALID_STATE) — a constraint
    // Arduino's Update.h did not have, so flashing within the 30 s before the
    // #76 rollback guard confirms would fail outright. Found by flashing twice
    // in a row; GhUpdate.cpp:171 already handles the same thing.
    //
    // Confirming here is honest rather than expedient: this code is running
    // inside an HTTP request that the image itself accepted, so it demonstrably
    // booted, joined the network and is serving. That is exactly the evidence
    // the guard waits for.
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t imgState;
    if (esp_ota_get_state_partition(running, &imgState) == ESP_OK &&
        imgState == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
      AssetUpdate::onImageConfirmed(); // keep the asset-backup bookkeeping in step
    }
    // Content-Length includes the multipart envelope, so it over-estimates the
    // image slightly — harmless, it only widens the erase. Clamp to the slot.
    size_t erase = sizeHint ? sizeHint : OTA_SIZE_UNKNOWN;
    if (erase != OTA_SIZE_UNKNOWN && erase > s_part->size) erase = s_part->size;
    esp_err_t e = esp_ota_begin(s_part, erase, &s_ota);
    if (e != ESP_OK) {
      s_error = esp_err_to_name(e);
      return false;
    }
  }
  s_running = true;
  return true;
}

static bool writerWrite(const uint8_t* data, size_t len) {
  if (!s_running) return false;
  if (!s_isFs) {
    esp_err_t e = esp_ota_write(s_ota, data, len);
    if (e != ESP_OK) {
      s_error = esp_err_to_name(e);
      return false;
    }
    return true;
  }
  if (s_fsOffset + len > s_part->size) {
    s_error = "image larger than the data partition";
    return false;
  }
  // Erase only as far as this write needs, rounded up to whole sectors.
  const size_t need = s_fsOffset + len;
  if (need > s_fsErased) {
    size_t end = (need + SPI_FLASH_SEC_SIZE - 1) & ~((size_t)SPI_FLASH_SEC_SIZE - 1);
    if (end > s_part->size) end = s_part->size;
    esp_err_t e = esp_partition_erase_range(s_part, s_fsErased, end - s_fsErased);
    if (e != ESP_OK) {
      s_error = esp_err_to_name(e);
      return false;
    }
    s_fsErased = end;
  }
  esp_err_t e = esp_partition_write(s_part, s_fsOffset, data, len);
  if (e != ESP_OK) {
    s_error = esp_err_to_name(e);
    return false;
  }
  s_fsOffset += len;
  return true;
}

// Commits. For firmware this is where the boot partition switches, which is
// the point of no return the onClose backstop reasons about.
static bool writerEnd() {
  if (!s_running) return false;
  s_running = false;
  if (s_isFs) return true; // a data partition has nothing to commit
  esp_err_t e = esp_ota_end(s_ota);
  if (e != ESP_OK) {
    s_error = esp_err_to_name(e);
    return false;
  }
  e = esp_ota_set_boot_partition(s_part);
  if (e != ESP_OK) {
    s_error = esp_err_to_name(e);
    return false;
  }
  return true;
}

static void writerAbort() {
  if (!s_running) return;
  s_running = false;
  if (!s_isFs) esp_ota_abort(s_ota); // frees the handle; slot stays unbootable
}

static bool writerRunning() { return s_running; }

static void fail(bool abortUpdate) {
  // s_error was set by whichever writer call failed; only fill in a fallback.
  if (s_error.empty()) s_error = "write failed";
  printf("[ota] %s\n", s_error.c_str());
  if (abortUpdate) writerAbort();
  s_outcome = Outcome::Failed;
  // fs-target failures keep the latch: LittleFS is unmounted, so the device
  // must reboot — either onUploadDone's Failed branch or, if the client
  // vanished first, the onClose backstop delivers that reboot.
  if (!s_isFs) s_inFlight = false;
}

static esp_err_t onUploadChunk(PsychicRequest* req, const char* filename,
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
    s_error.clear();
    Events::post(SysEvent::OtaStarting); // apps: stop drawing/allocating

    // ?target=fw (default) -> app partition; ?target=fs -> data partition.
    // Works as a form field only if it precedes the file part, so the query
    // string is the documented spelling.
    PsychicWebParameter* p = req->getParam("target");
    // p->value() is a const char* in PsychicHttp's native mode, so this MUST be
    // strcmp — == would compare pointers and silently always be false.
    s_isFs = (p != nullptr && strcmp(p->value(), "fs") == 0);
    printf("[ota] %s update starting (%s)\n",
                  s_isFs ? "filesystem" : "firmware", filename ? filename : "");

    if (s_isFs) {
      // Guard against flashing a non-filesystem image over the data partition
      // (nothing downstream verifies a data-partition image — a firmware.bin
      // here would destroy the filesystem). A littlefs image carries
      // "littlefs" at
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
      esp_vfs_littlefs_unregister("spiffs");
    } else {
      // Mirror of the littlefs guard for the fw path: every ESP32 app image
      // starts with 0xE9. esp_ota_write validates the header itself and would
      // fail on the first chunk anyway, but checking here gives an honest
      // error before the partition is erased — symmetric with the fs guard.
      if (len < 1 || data[0] != 0xE9) {
        s_error = "not an ESP32 app image (magic 0xE9 missing at byte 0)";
        s_outcome = Outcome::Failed;
        s_inFlight = false;
        return ESP_OK;
      }
    }

    // Content-Length bounds the erase; 0 (absent / chunked) falls back to
    // erasing the whole slot inside writerBegin.
    if (!writerBegin(req ? req->contentLength() : 0)) {
      fail(false);
      return ESP_OK;
    }
  }

  if (s_outcome != Outcome::None) return ESP_OK; // already decided; drain

  if (len > 0 && !writerWrite(data, len)) {
    fail(true);
    return ESP_OK;
  }
  s_progress += len;

  if (last) {
    if (writerEnd()) {
      printf("[ota] update complete: %u bytes\n", (unsigned)s_progress);
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
      Net::restartToApply(); // flushes the response, then Platform::restart(); no return
      return r;
    }
    case Outcome::Rejected:
      s_outcome = Outcome::None;
      return res->send(409, "application/json", "{\"error\":\"update already in progress\"}");
    case Outcome::Failed: {
      s_outcome = Outcome::None;
      std::string out = "{\"error\":\"";
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
      // A malformed body can abort the part callbacks after writerBegin()
      // succeeded (the processor drains bytes and still reports ESP_OK) —
      // without this cleanup the in-flight latch would block all future
      // updates until a power cycle.
      if (s_inFlight) {
        if (writerRunning()) writerAbort();
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
    printf("[ota] client gone after successful update; restarting\n");
    Net::restartToApply(); // no return
  }
  printf("[ota] upload aborted (client disconnect / parse error)\n");
  if (writerRunning()) writerAbort();
  s_inFlight = false;
  s_outcome = Outcome::None;
  if (s_isFs) Net::restartToApply(); // filesystem is unmounted/possibly torn; reboot
}

void registerRoutes(PsychicHttpServer& server) {
  // The library default MAX_UPLOAD_SIZE (2 MB) is smaller than the 2.17 MB
  // app partition; a near-full firmware image would be refused with a 400
  // before our handler ever ran. The partition bounds still enforce the real
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
