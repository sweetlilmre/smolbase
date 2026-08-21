// Smolbase tunables and hardware pins — the one header a consumer may need to tweak.
#pragma once

// ---- Identity ----
#define SMOLBASE_NAME_PREFIX "smolbase" // hostname/AP SSID become smolbase-XXXX (MAC suffix)
// Reported by GET /api/status; consumers set their own via build_flags -D.
#ifndef SMOLBASE_FW_VERSION
#define SMOLBASE_FW_VERSION "0.3.2"
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
// Boot calibration: average this many touchRead() samples with the pad untouched.
#define SMOLBASE_TOUCH_CAL_SAMPLES 16
// Pressed when the reading drops below (untouched baseline - margin).
#define SMOLBASE_TOUCH_MARGIN 120
// A boot baseline below this is implausible for an untouched pad (finger present
// at boot?) — fall back to the default threshold instead of miscalibrating.
#define SMOLBASE_TOUCH_MIN_BASELINE 200
#define SMOLBASE_TOUCH_DEFAULT_THRESHOLD 300
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
#define SMOLBASE_SETTINGS_PATH "/config/settings.json"
#define SMOLBASE_WWW_DIR "/w" // gzip-only static assets packed from html/
