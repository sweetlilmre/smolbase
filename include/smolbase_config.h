// Smolbase tunables and hardware pins — the one header a consumer may need to tweak.
#pragma once

// ---- Identity ----
#define SMOLBASE_NAME_PREFIX "smolbase" // hostname/AP SSID become smolbase-XXXX (MAC suffix)

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
// Framebuffer mode: 0 = none (direct draw), 1 = 8-bpp palette (57.6 KB static),
// 2 = RGB565 (115.2 KB, heap-allocated once at boot — too big for static DRAM).
#define SMOLBASE_FB_NONE 0
#define SMOLBASE_FB_PALETTE_8 1
#define SMOLBASE_FB_RGB565 2
#ifndef SMOLBASE_FRAMEBUFFER
#define SMOLBASE_FRAMEBUFFER SMOLBASE_FB_PALETTE_8
#endif

// ---- Network state machine (wayfinder ticket #8) ----
#define SMOLBASE_CONNECT_TIMEOUT_MS 20000 // boot: stored creds get this long, then AP mode
// Runtime WiFi drops auto-reconnect forever; there is deliberately no runtime AP fallback.

// ---- Main loop contract ----
#define SMOLBASE_LOOP_BUDGET_MS 25 // soft latency budget; debug builds log overruns

// ---- Paths ----
#define SMOLBASE_SETTINGS_PATH "/config/settings.json"
#define SMOLBASE_WWW_DIR "/w" // gzip-only static assets packed from html/
