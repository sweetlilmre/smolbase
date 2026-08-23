#include "Clock.h"
#include "ConfigStore.h"
#include "Events.h"
#include "Net.h"
#include "Platform.h"
#include "smolbase_config.h"
#include <lwip/apps/sntp.h>
#include <lwip/priv/tcpip_priv.h>
#include <atomic>
#include <cstdio>
#include <sys/time.h>

// The RAW lwIP SNTP API, under the lwIP core lock — deliberately, and
// deliberately WITHOUT <esp_sntp.h>.
//
// IDF's esp_sntp_init() and esp_sntp_stop() are both a bare
// tcpip_callback(...): a fire-and-forget post to the tcpip thread. Ticket #52
// was exactly that — a queued stop landing AFTER a fresh init killed the
// session (no pcb, no retry timer, clock stuck on "--:--"). Arduino's
// configTzTime, which kick() below replaces, avoided it by holding the lwIP
// core lock and calling the raw lwIP functions, which then run synchronously
// and in order. This is a faithful port of that, not a rewrite.
//
// <esp_sntp.h> is not included because it defines deprecated `static inline`
// shims that SHADOW the raw names — sntp_init, sntp_setservername,
// sntp_setoperatingmode — and forward them to the async esp_sntp_* variants.
// Including it would silently reintroduce #52 while every call site in kick()
// still read as the raw API. The one declaration genuinely needed from it is
// reproduced here instead.
extern "C" {
typedef void (*sntp_sync_time_cb_t)(struct timeval* tv);
void sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t callback);
}

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
// handed to kick(), so the running SNTP session only ever reads buffers nobody
// is writing. That removed a pre-copy esp_sntp_stop() from this function, and
// removing it is what fixed ticket #52: esp_sntp_stop() is a fire-and-forget
// tcpip_callback(do_stop) (verified by disassembly of the prebuilt wrapper),
// and when the tcpip thread was busy at boot the queued stop landed AFTER the
// fresh sntp_init and silently killed the session — no pcb, no retry timer,
// clock stuck until the #38 belt. The ping-pong stands on its own merits; the
// synchronous stop inside kick() is the other half of the same lesson.
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

// (Re)start SNTP against the active buffers. The stop happens synchronously,
// under the lwIP core lock, before the server list is touched — see the note at
// the top of this file, and the #52 note above.
static void kick() {
  sntp_set_time_sync_notification_cb(onSntpSync);
#if defined(CONFIG_LWIP_TCPIP_CORE_LOCKING)
  // Re-entrancy: this can be reached from a context that already holds the lock.
  const bool takeLock = !sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER);
  if (takeLock) LOCK_TCPIP_CORE();
#endif
  if (sntp_enabled()) sntp_stop();
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, ntpBufs[bufSel]);
  sntp_setservername(1, kNtpFallback1);
  sntp_setservername(2, kNtpFallback2);
  sntp_init();
#if defined(CONFIG_LWIP_TCPIP_CORE_LOCKING)
  if (takeLock) UNLOCK_TCPIP_CORE();
#endif
  setenv("TZ", tzBufs[bufSel], 1);
  tzset();
}

void begin() {
  // Nothing to register since ticket #57: timezone is a Choice setting owned
  // by ConfigStore::begin() — "tz" holds the POSIX value the firmware applies,
  // its derived "tz_name" the IANA label the UI picked from zones.json.
}

void sync() {
  // Safe to call repeatedly (NetworkUp after a drop, or a settings change while
  // online): kick() stops any running SNTP session, re-applies TZ via
  // setenv/tzset, sets the servers, and restarts SNTP. Default sync interval
  // (1h) is kept.
  std::string ntp = ConfigStore::getString("ntp"); // defaults live in the settings schema
  std::string tz = ConfigStore::getString("tz");

  // Skip the stop/restart churn (and the NTP-pool hammering) when nothing
  // that SNTP consumes actually changed — every settings save lands here.
  if (kicked && ntp == ntpBufs[bufSel] && tz == tzBufs[bufSel]) return;
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
  printf("[clock] no NTP sync within the re-kick window; restarting SNTP\n");
  kick();
}

void applyTimezone() {
  std::string tz = ConfigStore::getString("tz");
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
