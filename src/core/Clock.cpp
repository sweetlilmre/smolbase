#include "Clock.h"
#include "ConfigStore.h"
#include "Events.h"
#include "Net.h"
#include "Platform.h"
#include "smolbase_config.h"
#include <Arduino.h>
#include <atomic>
#include <esp_sntp.h>

namespace Clock {

// Fallback pool when the configured server is slow/unreachable. The primary comes
// from the "ntp" setting; SNTP tries these in order.
static const char* kNtpFallback1 = "time.nist.gov";
static const char* kNtpFallback2 = "time.google.com";

static std::atomic<bool> synced{false}; // written on the SNTP task, read on core 1

// SNTP stores RAW server pointers, so these must outlive every call and never
// be reallocated (a static String's reassignment frees the old buffer while
// the SNTP task may still dereference it — a real use-after-free window).
// Ping-pong pairs: new values are written into the INACTIVE pair before being
// handed to configTzTime, so the running SNTP session only ever reads buffers
// nobody is writing. This replaces an explicit pre-copy esp_sntp_stop() — that
// call is a fire-and-forget tcpip_callback(do_stop) (verified by disassembly
// of the prebuilt wrapper), and when the tcpip thread was busy at boot the
// queued stop landed AFTER configTzTime's fresh sntp_init and silently killed
// the session: no pcb, no retry timer, clock stuck until the #38 belt. That
// async stop was the root cause of ticket #52 — never call esp_sntp_stop()
// (or the sntp_stop() shim, which is the same call) from here.
static char ntpBufs[2][64];
static char tzBufs[2][64];
static uint8_t bufSel = 0;     // pair currently handed to SNTP
static bool kicked = false;    // sync() has started SNTP at least once
static uint32_t waitMs = 0;    // unsynced-while-online stopwatch (ticket #38)

bool isSynced() { return synced.load(); }

static void onSntpSync(struct timeval*) {
  // SNTP task context (core 0) — post only.
  synced.store(true);
  Events::post(SysEvent::TimeSynced);
}

// (Re)start SNTP against the active buffers. configTzTime stops any running
// session itself — synchronously, under the lwIP core lock — before touching
// the server list; no explicit stop belongs here (see the #52 note above).
static void kick() {
  sntp_set_time_sync_notification_cb(onSntpSync);
  configTzTime(tzBufs[bufSel], ntpBufs[bufSel], kNtpFallback1, kNtpFallback2);
}

void begin() {
  // Nothing to register since ticket #57: timezone is a Choice setting owned
  // by ConfigStore::begin() — "tz" holds the POSIX value the firmware applies,
  // its derived "tz_name" the IANA label the UI picked from zones.json.
}

void sync() {
  // Safe to call repeatedly (NetworkUp after a drop, or a settings change while
  // online): configTzTime stops any running SNTP session, re-applies TZ via
  // setenv/tzset, sets the servers, and restarts SNTP. Default sync interval
  // (1h) is kept.
  String ntp = ConfigStore::getString("ntp"); // defaults live in the settings schema
  String tz = ConfigStore::getString("tz");

  // Skip the stop/restart churn (and the NTP-pool hammering) when nothing
  // that SNTP consumes actually changed — every settings save lands here.
  if (kicked && ntp.equals(ntpBufs[bufSel]) && tz.equals(tzBufs[bufSel])) return;
  kicked = true;

  // Write into the inactive pair, then flip: the live session never sees a
  // buffer mid-overwrite, without ever stopping SNTP from this thread.
  uint8_t next = bufSel ^ 1;
  strlcpy(ntpBufs[next], ntp.c_str(), sizeof(ntpBufs[next]));
  strlcpy(tzBufs[next], tz.c_str(), sizeof(tzBufs[next]));
  bufSel = next;
  kick();
}

void loop() {
  // Re-kick belt (ticket #38): SNTP occasionally never delivers after a soft
  // restart (observed on-device; lwip's own retries notwithstanding), leaving
  // the screen on "--:--" until a power cycle. If we're online, SNTP has been
  // started, and no sync has landed within the window, restart the session —
  // converts a silent stall into at most a one-window outage.
  if (synced.load() || !kicked || !Net::isUp()) {
    waitMs = 0;
    return;
  }
  uint32_t now = Platform::millis();
  if (waitMs == 0) {
    waitMs = now;
    return;
  }
  if (now - waitMs < SMOLBASE_SNTP_REKICK_MS) return;
  waitMs = now;
  Serial.println("[clock] no NTP sync within the re-kick window; restarting SNTP");
  kick();
}

void applyTimezone() {
  String tz = ConfigStore::getString("tz");
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

bool nowLocal(struct tm& out) {
  if (!synced.load()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, &out);
  return true;
}

} // namespace Clock
