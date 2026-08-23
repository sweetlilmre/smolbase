// Smolbase tunables and hardware pins — the one header a consumer may need to tweak.
#pragma once

// ---- Identity ----
#define SMOLBASE_NAME_PREFIX "smolbase" // hostname/AP SSID become smolbase-XXXX (MAC suffix)
// Reported by GET /api/status; consumers set their own with a -D in CMake.
#ifndef SMOLBASE_FW_VERSION
// 0.4.0-dev: the IDF 6 migration branch. Unreleased, and deliberately distinct
// from 0.3.3 so a flashed build is identifiable over /api/status.
#define SMOLBASE_FW_VERSION "0.4.0-dev"
#endif

// ---- Hardware: GeekMagic Small TV Pro (ESP32-D0WD, 8MB flash, no PSRAM) ----
#define SMOLBASE_PIN_SCLK 18
#define SMOLBASE_PIN_MOSI 23
#define SMOLBASE_PIN_DC 2
#define SMOLBASE_PIN_RST 4
#define SMOLBASE_PIN_BL 25   // active-LOW PWM
#define SMOLBASE_PIN_TOUCH 32 // capacitive pad (T9)

// ---- Display ----
// 60 MHz is not an achievable classic-ESP32 SPI divider; 40 MHz is the proven safe rate.
#define SMOLBASE_SPI_HZ 40000000
// Framebuffer mode: 0 = none (direct draw), 1 = 8-bpp palette, 2 = 8-bpp
// true-color RGB332 (#102). Both 8-bpp modes share the same 57.6 KB static
// buffer. PALETTE_8 treats color arguments as RAW palette indices — that
// enables palette-cycling animation (BoingScreen) but makes every COMPUTED
// color write garbage: anti-aliased glyph blends and pushImage conversions
// produce RGB values, not indices. RGB332 is a true-color sprite: all writes
// convert correctly (AA text quantizes to real colors, RGB565 icons convert),
// with no palette tricks available. Pick per env with -DSMOLBASE_FRAMEBUFFER.
// There is deliberately no RGB565 mode: 115.2 KB doesn't fit static DRAM, and the
// heap fallback ate most of the WiFi-era headroom on this no-PSRAM chip. Consumers
// needing full-color composition should use a partial-frame 16-bpp sprite instead.
#define SMOLBASE_FB_NONE 0
#define SMOLBASE_FB_PALETTE_8 1
#define SMOLBASE_FB_RGB332 2
#ifndef SMOLBASE_FRAMEBUFFER
#define SMOLBASE_FRAMEBUFFER SMOLBASE_FB_PALETTE_8
#endif

// If SNTP hasn't delivered a sync this long after coming online, restart the
// session (ticket #38: a soft restart can leave SNTP silently stalled).
#define SMOLBASE_SNTP_REKICK_MS 60000

// ---- Network state machine (wayfinder ticket #8) ----
#define SMOLBASE_CONNECT_TIMEOUT_MS 20000 // boot: stored creds get this long, then AP mode
// Runtime WiFi drops auto-reconnect forever; there is deliberately no runtime AP fallback.

// ---- Touch (single capacitive pad, T9/GPIO32) ----
// The touch-sensor driver's channel id for GPIO32. Kept alongside the pin so
// the two cannot drift: SMOLBASE_PIN_TOUCH is documentation now, since the
// driver addresses the pad by channel.
#define SMOLBASE_TOUCH_CHANNEL 9
// V1 sample config: charge duration in ms (float). The driver's own test app
// uses 5.0 for this hardware revision.
#define SMOLBASE_TOUCH_CHARGE_MS 5.0f
// How often the driver's software filter samples the pad. The filter is a
// PASS-THROUGH (see Touch.cpp note 3), so this is pure sampling cadence and
// sets the floor on how fast an edge can be seen. 10 ms is the driver default;
// 5 ms halves the edge latency at negligible cost, which matters because a
// quick tap must be seen on BOTH edges before onTap fires.
#define SMOLBASE_TOUCH_FILTER_MS 5
// Wait at least this long after starting the scan before the first read, or
// calibration samples come back 0.
#define SMOLBASE_TOUCH_SETTLE_MS 100
// Boot calibration: average this many samples with the pad untouched, spaced
// far enough apart that the sampler has moved on between reads.
#define SMOLBASE_TOUCH_CAL_SAMPLES 16
#define SMOLBASE_TOUCH_CAL_GAP_MS 20
// Pressed when the reading falls this far BELOW the measured baseline, as a
// PERCENTAGE of it. A percentage rather than an absolute count because the
// driver's value scale is nothing like Arduino touchRead()'s (~1600 vs a few
// hundred on this pad), so an absolute margin does not survive the port.
//
// Calibrated from measurement, not guessed. On this device:
//   untouched  1605-1608  (noise +/-3, ~0.2%)
//   held       1262-1277  (a 20.4-21.4% drop)
// 10% puts the threshold at ~1445, almost exactly midway between the two —
// ~160 counts clear of the untouched level (53x the observed noise, so false
// presses are implausible) and ~168 clear of a held reading, which leaves room
// for a lighter or drier touch than the one measured. 15% was the first guess
// and worked for a firm press, but sat close enough to the held level to risk
// missing a light one — and this pad is the device's ONLY input, so a missed
// touch is worse than a rare spurious one that the debounce would filter.
//
// /api/status reports touchBaseline / touchThreshold / touchNow for re-tuning.
// Caveat: calibration is single-shot at boot (as it was under Arduino), so a
// tighter margin also leaves less headroom for baseline drift.
#define SMOLBASE_TOUCH_DELTA_PCT 10
// An edge (press or release) must hold this long before it is believed.
#define SMOLBASE_TOUCH_DEBOUNCE_MS 30
// Held at least this long = long-press (fires once while held); shorter = tap on release.
#define SMOLBASE_TOUCH_LONGPRESS_MS 600

// ---- Main loop contract ----
#define SMOLBASE_LOOP_BUDGET_MS 25 // soft latency budget; debug builds log overruns

// ---- Settings schema ----
#ifndef SMOLBASE_MAX_SETTINGS
#define SMOLBASE_MAX_SETTINGS 24 // static registry capacity (system + app entries)
#endif

// ---- Paths ----
// One spelling. The LittleFS volume is mounted at the ROOT (ConfigStore::begin),
// so a path is simultaneously the POSIX path core/Fs.h wants, the path
// PsychicHttp's native serveStatic() wants, and the URL suffix — nothing has to
// be rewritten at a boundary and there is no prefixed/unprefixed pair to get
// wrong. SMOLBASE_FS_MOUNT is kept, empty, because it is what the mount is
// configured with and because the concatenated forms below still read clearly.
//
// (The Arduino build mounted at "/littlefs" and every path existed in two
// spellings; findBackupDir's `strncmp(path, "/w.", 3)` silently assumed the
// unprefixed one, which is the class of bug this removes.)
#define SMOLBASE_FS_MOUNT ""
#define SMOLBASE_SETTINGS_PATH "/config/settings.json"
#define SMOLBASE_SETTINGS_FSPATH SMOLBASE_FS_MOUNT SMOLBASE_SETTINGS_PATH
#define SMOLBASE_WWW_DIR "/w" // gzip-only static assets packed from html/
#define SMOLBASE_WWW_FSPATH SMOLBASE_FS_MOUNT SMOLBASE_WWW_DIR
