// Phase 0 spike — native ESP-IDF 6 on the GeekMagic Small TV Pro.
// See README.md for what each check retires and why flashing this is safe.
//
// SAFETY: this app must NEVER call esp_ota_mark_app_valid_cancel_rollback().
// It runs as ESP_OTA_IMG_PENDING_VERIFY forever, so a power-cycle rolls the
// device back to the real firmware. That is the only recovery path on a bench
// with no serial flasher. Do not "fix" this.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_idf_version.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <dirent.h>

#include "driver/touch_sens.h"

#include <ArduinoJson.h>
#include <PsychicHttp.h>

#include "SpikePanel.hpp"
#include "sb_fs.h"

static const char* TAG = "spike";

// ---------------------------------------------------------------- results ----

enum class State { Pending, Pass, Fail, Skip };

struct Check {
  const char* name;
  State state;
  std::string note;
  // Explicit ctor rather than aggregate init: IDF builds with
  // -Werror=missing-field-initializers, so `{"name"}` is a hard error.
  explicit Check(const char* n) : name(n), state(State::Pending), note() {}
};

static Check checks[] = {
    Check("1 nvs creds"),   Check("2 littlefs /w"),     Check("3 panel init"),
    Check("4 band dma"),    Check("5 touch t9"),        Check("6 wifi join"),
    Check("7 tls api"),     Check("8 tls cdn rsa4096"), Check("9 psychic idf"),
    Check("10 footprint"),  Check("11 sb_fs wrapper"),  Check("12 root mount"),
};
static constexpr int CHECK_COUNT = sizeof(checks) / sizeof(checks[0]);

static SpikePanel panel;
static bool panelUp = false;

static const char* stateStr(State s) {
  switch (s) {
    case State::Pass: return "PASS";
    case State::Fail: return "FAIL";
    case State::Skip: return "SKIP";
    default: return "....";
  }
}

// Redraws the whole list. Cheap enough at 10 rows, and a full repaint avoids
// any dependency on dirty-rect logic we have not proven yet.
static void drawResults() {
  if (!panelUp) return;
  panel.fillScreen(0x0000);
  panel.setTextColor(0xFFFF, 0x0000);
  panel.setTextSize(1);
  panel.setCursor(2, 2);
  panel.printf("IDF %s\n", esp_get_idf_version());
  // 12 rows at an 18 px pitch from y=12: last row lands at 210, its note at
  // 219 — inside 240. Do not add rows without re-checking this.
  for (int i = 0; i < CHECK_COUNT; ++i) {
    uint16_t c = 0xFFFF;                              // white  = pending
    if (checks[i].state == State::Pass) c = 0x07E0;   // green
    if (checks[i].state == State::Fail) c = 0xF800;   // red
    if (checks[i].state == State::Skip) c = 0x7BEF;   // grey
    panel.setTextColor(c, 0x0000);
    panel.setCursor(2, 12 + i * 18);
    panel.printf("%-4s %s", stateStr(checks[i].state), checks[i].name);
    if (!checks[i].note.empty()) {
      panel.setCursor(2, 12 + i * 18 + 9);
      panel.setTextColor(0x8410, 0x0000);
      panel.printf("     %.28s", checks[i].note.c_str());
    }
  }
}

static void report(int idx, State s, const std::string& note = "") {
  checks[idx].state = s;
  checks[idx].note = note;
  ESP_LOGI(TAG, "[%s] %s -- %s", stateStr(s), checks[idx].name, note.c_str());
  drawResults();
}

// A failing ESP_ERROR_CHECK panics, reboots, and rolls back — the device is
// safe, but all twelve results are lost. On a bench where each flash needs a
// human's say-so that is far too expensive, so every fallible call reports and
// bails out of its own check instead of aborting the run.
#define SB_TRY(idx, label, expr)                                                    \
  do {                                                                             \
    esp_err_t _e = (expr);                                                          \
    if (_e != ESP_OK) {                                                             \
      report((idx), State::Fail, std::string(label ": ") + esp_err_to_name(_e));     \
      return;                                                                       \
    }                                                                               \
  } while (0)

static std::string heapLine() {
  char buf[96];
  snprintf(buf, sizeof(buf), "free %u largest %u min %u",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)esp_get_minimum_free_heap_size());
  return buf;
}

// ------------------------------------------------- 1. NVS credentials --------
// The shipped firmware writes these with Arduino Preferences (namespace
// "smolbase", keys "ssid"/"pass"). Preferences::putString is nvs_set_str
// underneath, so plain nvs_get_str should read them back. If it does, fielded
// credentials survive the framework change and the Phase 6 rewrite does not
// need a migration step.

static std::string wifiSsid, wifiPass;

static void checkNvsCreds() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // Do NOT erase: that would destroy the device's real credentials.
    report(0, State::Fail, "nvs needs erase - refusing");
    return;
  }
  if (err != ESP_OK) {
    report(0, State::Fail, esp_err_to_name(err));
    return;
  }

  nvs_handle_t h;
  err = nvs_open("smolbase", NVS_READONLY, &h);
  if (err != ESP_OK) {
    report(0, State::Fail, std::string("nvs_open: ") + esp_err_to_name(err));
    return;
  }

  char buf[128];
  size_t len = sizeof(buf);
  if (nvs_get_str(h, "ssid", buf, &len) == ESP_OK) wifiSsid = buf;
  len = sizeof(buf);
  if (nvs_get_str(h, "pass", buf, &len) == ESP_OK) wifiPass = buf;
  nvs_close(h);

  if (wifiSsid.empty()) {
    report(0, State::Fail, "no ssid in nvs");
    return;
  }
  // Never log the password, and never log more of the SSID than needed to
  // confirm the read worked.
  char note[64];
  snprintf(note, sizeof(note), "ssid len %u, pass %s", (unsigned)wifiSsid.size(),
           wifiPass.empty() ? "empty" : "present");
  report(0, State::Pass, note);
}

// ------------------------------------------------------- 2. LittleFS ---------
// Mounts the live asset volume. The partition is labelled `spiffs` but holds
// LittleFS (see the root partitions.csv comment).

static void checkLittlefs() {
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = "/lfs";
  conf.partition_label = "spiffs";
  conf.format_if_mount_failed = false; // never reformat the live volume
  conf.dont_mount = false;

  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    report(1, State::Fail, std::string("register: ") + esp_err_to_name(err));
    return;
  }

  size_t total = 0, used = 0;
  esp_littlefs_info(conf.partition_label, &total, &used);

  // Walked with sb::fs::Dir — this is the wrapper's directory iterator under
  // test, and it also proves esp_littlefs populates d_type (the isDirectory()
  // question from the sketch): if d_type were DT_UNKNOWN, dirsAtRoot would be 0
  // even though /w demonstrably exists.
  int files = 0;
  sb::fs::Dir w("/lfs/w");
  sb::fs::Dir::Entry e;
  while (w.next(e)) {
    if (!e.isDir) ++files;
  }

  int dirsAtRoot = 0;
  sb::fs::Dir root("/lfs");
  while (root.next(e)) {
    if (e.isDir) ++dirsAtRoot;
  }

  char note[90];
  snprintf(note, sizeof(note), "%d files /w, %d dirs /, %u/%u KB", files, dirsAtRoot,
           (unsigned)(used / 1024), (unsigned)(total / 1024));
  report(1, files > 0 && dirsAtRoot > 0 ? State::Pass : State::Fail, note);
}

// -------------------------------------------- 11. sb_fs wrapper -------------
// Exercises every part of sb_fs.h against the live volume: path ops (bool
// sense), the RAII File, and ArduinoJson streaming straight through the handle
// — the concept-detection question from the sketch, which is really a
// compile-time claim that this makes a runtime one too.
//
// Confined to /lfs/.sbfs_test* — it never touches /w or /config.

static void checkSbFs() {
  if (checks[1].state != State::Pass) {
    report(10, State::Skip, "fs not mounted");
    return;
  }
  const char* kTmp = "/lfs/.sbfs_test";
  const char* kDst = "/lfs/.sbfs_test2";
  sb::fs::remove(kTmp); // clean slate; false here is fine (may not exist)
  sb::fs::remove(kDst);

  // --- RAII File + ArduinoJson writer concept ---
  {
    sb::fs::File f(kTmp, "w");
    if (!f) {
      report(10, State::Fail, "open for write failed");
      return;
    }
    JsonDocument doc;
    doc["hello"] = "sb_fs";
    doc["n"] = 42;
    if (serializeJson(doc, f) == 0) { // <- the concept claim, at runtime
      report(10, State::Fail, "serializeJson wrote 0");
      return;
    }
  } // closes here, by scope, with no explicit close() — the whole point

  // --- ArduinoJson reader concept + size() ---
  long sz = -1;
  {
    sb::fs::File f(kTmp, "r");
    if (!f) {
      report(10, State::Fail, "reopen failed");
      return;
    }
    sz = f.size();
    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok ||
        doc["n"].as<int>() != 42) {
      report(10, State::Fail, "roundtrip mismatch");
      return;
    }
  }

  // --- Path ops, checking the bool sense is right way round ---
  if (!sb::fs::exists(kTmp)) {
    report(10, State::Fail, "exists() false on real file");
    return;
  }
  if (!sb::fs::rename(kTmp, kDst)) {
    report(10, State::Fail, "rename returned false");
    return;
  }
  if (sb::fs::exists(kTmp) || !sb::fs::exists(kDst)) {
    report(10, State::Fail, "rename did not move it");
    return;
  }
  // mkdir must be a no-op on an existing dir (the Web.cpp:267 dependency).
  if (!sb::fs::mkdir("/lfs/w")) {
    report(10, State::Fail, "mkdir on existing dir returned false");
    return;
  }
  // ...and remove() must report failure honestly on something absent.
  if (sb::fs::remove("/lfs/.no_such_file")) {
    report(10, State::Fail, "remove() true on missing file");
    return;
  }
  if (!sb::fs::remove(kDst)) {
    report(10, State::Fail, "remove returned false");
    return;
  }
  if (sb::fs::exists(kDst)) {
    report(10, State::Fail, "remove left the file");
    return;
  }

  char note[80];
  snprintf(note, sizeof(note), "json %ld B roundtrip, path ops ok", sz);
  report(10, State::Pass, note);
}

// ------------------------------------------- 12. root mount -----------------
// From components/vfs/vfs.c: `if (base_path_len != 0 && !is_path_prefix_valid(
// ...))` — a zero-length base_path skips prefix validation, so mounting at "/"
// is legal as far as the VFS is concerned. If it also works in practice, the
// sketch's whole path problem dissolves: every existing constant ("/w",
// "/config/settings.json") stays byte-identical and there is no second path
// namespace to collide with PsychicHttp.
//
// Deliberately LAST: it registers a filesystem as the catch-all prefix, which
// could plausibly shadow the console's /dev/uart paths. Everything else has
// already reported to the panel and HTTP by now, so if this bricks the run we
// still have the results — and a power-cycle rolls back anyway.

static void checkRootMount() {
  if (checks[1].state != State::Pass) {
    report(11, State::Skip, "fs not mounted");
    return;
  }
  // MUST unmount /lfs first. esp_littlefs guards duplicate mounts by label
  // ("Partition already used" -> ESP_ERR_INVALID_STATE, esp_littlefs.c:1420),
  // so registering a second time while /lfs is live would fail on the guard
  // and prove nothing. The guard is also why this is not a corruption risk:
  // two concurrent littlefs instances on one partition cannot happen.
  esp_err_t err = esp_vfs_littlefs_unregister("spiffs");
  if (err != ESP_OK) {
    report(11, State::Fail, std::string("unregister /lfs: ") + esp_err_to_name(err));
    return;
  }

  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = ""; // the experiment
  conf.partition_label = "spiffs";
  conf.format_if_mount_failed = false; // never reformat the live volume
  conf.dont_mount = false;

  err = esp_vfs_littlefs_register(&conf);
  bool ok = false;
  std::string note;
  if (err != ESP_OK) {
    // Most likely ESP_ERR_INVALID_ARG, if joltwallet rejects an empty
    // base_path even though the VFS layer would accept it.
    note = std::string("register \"\": ") + esp_err_to_name(err);
  } else {
    ok = sb::fs::isDir("/w"); // does an unprefixed path resolve?
    note = ok ? "\"/w\" resolves unprefixed" : "mounted but /w not found";
    esp_vfs_littlefs_unregister(conf.partition_label);
  }

  // Restore the /lfs mount regardless, so the device is left as we found it.
  esp_vfs_littlefs_conf_t back = {};
  back.base_path = "/lfs";
  back.partition_label = "spiffs";
  back.format_if_mount_failed = false;
  if (esp_vfs_littlefs_register(&back) != ESP_OK) note += " (remount failed!)";

  report(11, ok ? State::Pass : State::Fail, note);
}

// ------------------------------------------------------- 3/4. Display -------

static void checkPanel() {
  panel.init();
  panel.setBrightness(200);
  panel.fillScreen(0x0000);
  panelUp = true;

  char note[64];
  // width()/height() are int32_t (long int here), so cast — IDF builds with
  // -Werror=format=.
  snprintf(note, sizeof(note), "%dx%d", (int)panel.width(), (int)panel.height());
  if (panel.width() != 240 || panel.height() != 240) {
    report(2, State::Fail, note);
    return;
  }
  report(2, State::Pass, note);
}

// ADR 0004's hot path, mirrored from WeatherScreen.cpp: a static 240x64 RGB565
// sprite bound with setBuffer (no heap sprite) and pushed with pushSprite. This
// is what actually exercises SPI DMA at 40 MHz. If DMA silently fell back to
// polled writes under IDF 6, the timing here is where it shows.
static constexpr int BAND_W = 240;
static constexpr int BAND_H = 64;
static uint8_t scratchData[BAND_W * BAND_H * 2];
static lgfx::LGFX_Sprite scratch;

static void checkBandPush() {
  if (!panelUp) {
    report(3, State::Skip, "panel down");
    return;
  }
  scratch.setColorDepth(16);
  scratch.setBuffer(scratchData, BAND_W, BAND_H, 16); // static — no heap sprite
  // Fill the backing store directly: the timed part below is pushSprite, and a
  // raw fill keeps this setup off any LovyanGFX API we have not verified yet.
  auto* px = reinterpret_cast<uint16_t*>(scratchData);
  for (int y = 0; y < BAND_H; ++y)
    for (int x = 0; x < BAND_W; ++x) px[y * BAND_W + x] = (uint16_t)((x << 5) ^ (y << 11));

  const int reps = 20;
  int64_t t0 = esp_timer_get_time();
  for (int i = 0; i < reps; ++i) scratch.pushSprite(&panel, 0, 176);
  panel.waitDMA();
  int64_t us = (esp_timer_get_time() - t0) / reps;

  // 240*64*2 bytes = 30720 B. At 40 MHz that is ~6.1 ms of pure bus time, so
  // anything near that is DMA working. Several times that means polled writes.
  char note[64];
  snprintf(note, sizeof(note), "%lld us/band (bus floor ~6100)", us);
  report(3, us < 15000 ? State::Pass : State::Fail, note);
}

// ---------------------------------------------------------- 5. Touch --------
// GPIO32 is touch channel T9. V1 hardware (ESP32): the channel value DECREASES
// when touched, and only absolute thresholds are supported.

static touch_sensor_handle_t touchSens = nullptr;
static touch_channel_handle_t touchChan = nullptr;
static constexpr int TOUCH_CHAN_ID = 9; // T9 == GPIO32

static void checkTouch() {
  // Values taken from IDF 6.0.2's own V1 test app
  // (components/esp_driver_touch_sens/test_apps/touch_sens/main/
  // test_touch_sens_common.cpp): 5.0 ms charge, 0V5 low / 1V7 high. Note V1
  // takes a float duration in ms, and its high limit is 1V7, not V2's 2V2.
  touch_sensor_sample_config_t sample[TOUCH_SAMPLE_CFG_NUM] = {
      TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(5.0, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_1V7),
  };
  touch_sensor_config_t sensCfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, sample);

  esp_err_t err = touch_sensor_new_controller(&sensCfg, &touchSens);
  if (err != ESP_OK) {
    report(4, State::Fail, std::string("new_controller: ") + esp_err_to_name(err));
    return;
  }

  // Absolute threshold, mirroring Touch.cpp: we calibrate below, so start with
  // the in-code default (SMOLBASE_TOUCH_DEFAULT_THRESHOLD).
  touch_channel_config_t chanCfg = {};
  chanCfg.abs_active_thresh[0] = 300;

  err = touch_sensor_new_channel(touchSens, TOUCH_CHAN_ID, &chanCfg, &touchChan);
  if (err != ESP_OK) {
    report(4, State::Fail, std::string("new_channel: ") + esp_err_to_name(err));
    return;
  }

  // The software filter is MANDATORY on V1, for a reason that is an upstream
  // bug rather than a design choice. hw_ver1/touch_version_specific.c:253:
  //   ESP_RETURN_ON_FALSE_ISR(type == TOUCH_CHAN_DATA_TYPE_SMOOTH &&
  //                           chan_handle->base->data_filter_fn != NULL, ...)
  // That guard should read `type != SMOOTH || filter != NULL` — as written it
  // rejects every RAW read with ESP_ERR_INVALID_STATE, unconditionally. So on
  // IDF 6.0.2 the only readable type is SMOOTH, and only once a filter exists.
  // Passing data_filter_fn = NULL installs the driver's default filter
  // (line 353: `filter_cfg->data_filter_fn ? ... : <default>`), which is what
  // makes the guard's second clause true.
  touch_sensor_filter_config_t filterCfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
  SB_TRY(4, "config_filter", touch_sensor_config_filter(touchSens, &filterCfg));

  SB_TRY(4, "enable", touch_sensor_enable(touchSens));
  SB_TRY(4, "start_scan", touch_sensor_start_continuous_scanning(touchSens));

  // Let the filter timer (10 ms interval) populate smooth_data before reading.
  vTaskDelay(pdMS_TO_TICKS(100));

  // Probe RAW once purely to document the bug empirically in the results.
  uint32_t rawProbe[TOUCH_SAMPLE_CFG_NUM] = {0};
  esp_err_t rawErr =
      touch_channel_read_data(touchChan, TOUCH_CHAN_DATA_TYPE_RAW, rawProbe);

  // Same boot calibration as Touch.cpp: average several untouched reads.
  uint32_t sum = 0;
  int got = 0;
  esp_err_t lastErr = ESP_OK;
  for (int i = 0; i < 16; ++i) {
    uint32_t sm[TOUCH_SAMPLE_CFG_NUM] = {0};
    lastErr = touch_channel_read_data(touchChan, TOUCH_CHAN_DATA_TYPE_SMOOTH, sm);
    if (lastErr == ESP_OK && sm[0]) {
      sum += sm[0];
      ++got;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (!got) {
    report(4, State::Fail, std::string("smooth reads gave 0, last=") + esp_err_to_name(lastErr));
    return;
  }
  uint32_t baseline = sum / got;
  char note[110];
  snprintf(note, sizeof(note), "smooth baseline %u (%d/16); RAW=%s (upstream bug)",
           (unsigned)baseline, got, esp_err_to_name(rawErr));
  // A plausible baseline is the whole question. Touch.cpp treats <200 as
  // implausible for an untouched pad.
  report(4, baseline >= 200 ? State::Pass : State::Fail, note);
}

// ----------------------------------------------------------- 6. WiFi -------

static EventGroupHandle_t wifiEvents;
static constexpr int WIFI_GOT_IP = BIT0;
static constexpr int WIFI_FAILED = BIT1;
static int wifiRetries = 0;
static esp_ip4_addr_t wifiIp = {};

static void wifiEventHandler(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    if (++wifiRetries <= 5) {
      esp_wifi_connect();
    } else {
      xEventGroupSetBits(wifiEvents, WIFI_FAILED);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto* e = (ip_event_got_ip_t*)data;
    wifiIp = e->ip_info.ip;
    xEventGroupSetBits(wifiEvents, WIFI_GOT_IP);
  }
}

static void checkWifi() {
  if (wifiSsid.empty()) {
    report(5, State::Skip, "no creds from nvs");
    return;
  }

  wifiEvents = xEventGroupCreate();
  SB_TRY(5, "netif_init", esp_netif_init());
  SB_TRY(5, "event_loop", esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
  SB_TRY(5, "wifi_init", esp_wifi_init(&initCfg));
  SB_TRY(5, "reg_wifi_ev", esp_event_handler_instance_register(
                               WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, nullptr, nullptr));
  SB_TRY(5, "reg_ip_ev", esp_event_handler_instance_register(
                             IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEventHandler, nullptr, nullptr));

  wifi_config_t staCfg = {};
  snprintf((char*)staCfg.sta.ssid, sizeof(staCfg.sta.ssid), "%s", wifiSsid.c_str());
  snprintf((char*)staCfg.sta.password, sizeof(staCfg.sta.password), "%s", wifiPass.c_str());

  SB_TRY(5, "set_mode", esp_wifi_set_mode(WIFI_MODE_STA));
  SB_TRY(5, "set_config", esp_wifi_set_config(WIFI_IF_STA, &staCfg));
  // Credentials live in our own NVS namespace; do not let esp_wifi keep a copy.
  SB_TRY(5, "set_storage", esp_wifi_set_storage(WIFI_STORAGE_RAM));
  SB_TRY(5, "wifi_start", esp_wifi_start());

  // SMOLBASE_CONNECT_TIMEOUT_MS
  EventBits_t bits = xEventGroupWaitBits(wifiEvents, WIFI_GOT_IP | WIFI_FAILED, pdFALSE,
                                         pdFALSE, pdMS_TO_TICKS(20000));
  if (!(bits & WIFI_GOT_IP)) {
    report(5, State::Fail, bits & WIFI_FAILED ? "disconnected x5" : "timeout 20s");
    return;
  }

  wifi_ap_record_t ap = {};
  esp_wifi_sta_get_ap_info(&ap);
  char note[80];
  snprintf(note, sizeof(note), IPSTR " rssi %d", IP2STR(&wifiIp), ap.rssi);
  report(5, State::Pass, note);
}

// -------------------------------------------------------- 7/8. TLS ---------
// Both checks use esp_http_client + esp_crt_bundle, which is the shape
// AssetUpdate::fetchTar and GhUpdate already use today. Check 8 is the one that
// matters: the release-asset URL 302s to the Fastly CDN whose chain is
// cross-signed by RSA-4096 ISRG Root X1, and without
// CONFIG_MBEDTLS_LARGE_KEY_SOFTWARE_MPI it dies with PK error 0x4290 (#119).

static constexpr const char* GH_REPO = "sweetlilmre/smolbase";

struct FetchResult {
  esp_err_t err = ESP_FAIL;
  int status = 0;
  int bytesRead = 0;
  uint32_t heapFloor = 0;
};

static FetchResult tlsFetch(const char* url, int maxBytes, bool followRedirect,
                            char* sink = nullptr, size_t sinkLen = 0) {
  FetchResult r;
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 15000;
  cfg.disable_auto_redirect = followRedirect; // we drive the 302 by hand

  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return r;
  esp_http_client_set_header(c, "User-Agent", "smolbase-idf6-spike");

  r.heapFloor = esp_get_free_heap_size();

  for (int hop = 0; hop < 3; ++hop) {
    r.err = esp_http_client_open(c, 0);
    if (r.err != ESP_OK) break;
    esp_http_client_fetch_headers(c);
    r.status = esp_http_client_get_status_code(c);
    uint32_t now = esp_get_free_heap_size();
    if (now < r.heapFloor) r.heapFloor = now;

    if (followRedirect && (r.status == 301 || r.status == 302 || r.status == 307)) {
      esp_http_client_set_redirection(c);
      continue;
    }
    break;
  }

  if (r.err == ESP_OK && r.status == 200) {
    char buf[512];
    size_t sinkUsed = 0;
    if (sink && sinkLen) sink[0] = '\0';
    while (r.bytesRead < maxBytes) {
      int n = esp_http_client_read(c, buf, sizeof(buf));
      if (n <= 0) break;
      if (sink && sinkUsed + (size_t)n + 1 < sinkLen) {
        memcpy(sink + sinkUsed, buf, n);
        sinkUsed += n;
        sink[sinkUsed] = '\0';
      }
      r.bytesRead += n;
      uint32_t now = esp_get_free_heap_size();
      if (now < r.heapFloor) r.heapFloor = now;
    }
  }

  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return r;
}

static uint32_t tlsHeapFloor = 0;
static std::string releaseTag;

static void checkTlsApi() {
  if (checks[5].state != State::Pass) {
    report(6, State::Skip, "no network");
    return;
  }
  std::string url = std::string("https://api.github.com/repos/") + GH_REPO + "/releases/latest";
  // Capture the body so check 8 can build a URL for an asset that actually
  // exists. "tag_name" appears early in the GitHub release JSON.
  static char body[2048];
  FetchResult r = tlsFetch(url.c_str(), sizeof(body) - 1, false, body, sizeof(body));
  tlsHeapFloor = r.heapFloor;

  if (const char* p = strstr(body, "\"tag_name\":\"")) {
    p += strlen("\"tag_name\":\"");
    const char* e = strchr(p, '"');
    if (e && e > p) releaseTag.assign(p, e - p);
  }

  char note[80];
  snprintf(note, sizeof(note), "http %d, %d B, floor %u", r.status, r.bytesRead,
           (unsigned)r.heapFloor);
  report(6, r.err == ESP_OK && r.status == 200 && r.bytesRead > 0 ? State::Pass : State::Fail,
         note);
}

static void checkTlsCdn() {
  if (checks[6].state != State::Pass) {
    report(7, State::Skip, "api fetch failed");
    return;
  }
  if (releaseTag.empty()) {
    report(7, State::Skip, "no tag_name parsed");
    return;
  }
  // The asset URL shape GhUpdate builds: <prefix>-<tag>.bin. Getting this WRONG
  // is how the first run of this check produced a false pass — a bare
  // "smolbase-firmware.bin" 404s at github.com and never redirects, so the
  // handshake that succeeded was github.com's, not the CDN's. Nothing below
  // status 200 WITH bytes read proves the RSA-4096 chain verified.
  std::string url = std::string("https://github.com/") + GH_REPO + "/releases/download/" +
                    releaseTag + "/smolbase-firmware-" + releaseTag + ".bin";
  FetchResult r = tlsFetch(url.c_str(), 8192, true);
  if (r.heapFloor < tlsHeapFloor) tlsHeapFloor = r.heapFloor;

  char note[128];
  snprintf(note, sizeof(note), "%s http %d, %d B, floor %u", releaseTag.c_str(), r.status,
           r.bytesRead, (unsigned)r.heapFloor);
  // 200 AND bytes: the redirect to the CDN was followed and its chain verified.
  report(7, r.err == ESP_OK && r.status == 200 && r.bytesRead > 0 ? State::Pass : State::Fail,
         note);
}

// ------------------------------------------------- 9. PsychicHttp native ----
// Native (non-Arduino) mode: std::string, not String. This is also how the
// results leave the device, so it is deliberately the last functional check.

static PsychicHttpServer server;

static void resultsJson(JsonDocument& doc) {
  doc["idf"] = esp_get_idf_version();
  doc["heapFree"] = esp_get_free_heap_size();
  doc["heapLargest"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  doc["heapMinEver"] = esp_get_minimum_free_heap_size();
  doc["tlsHeapFloor"] = tlsHeapFloor;
  doc["mainTaskHighWater"] = uxTaskGetStackHighWaterMark(nullptr);
  doc["uptimeS"] = (uint32_t)(esp_timer_get_time() / 1000000);
  if (const esp_app_desc_t* d = esp_app_get_description()) doc["appName"] = d->project_name;
  JsonArray arr = doc["checks"].to<JsonArray>();
  for (int i = 0; i < CHECK_COUNT; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = checks[i].name;
    o["state"] = stateStr(checks[i].state);
    o["note"] = checks[i].note;
  }
}

static void checkPsychic() {
  if (checks[5].state != State::Pass) {
    report(8, State::Skip, "no network");
    return;
  }
  server.config.core_id = 0;
  server.config.lru_purge_enable = true;
  server.setPort(80);

  server.on("/", HTTP_GET, [](PsychicRequest*, PsychicResponse* res) -> esp_err_t {
    JsonDocument doc;
    resultsJson(doc);
    std::string out;
    serializeJson(doc, out);
    return res->send(200, "application/json", out.c_str());
  });

  // Remote recovery. Run 1 shipped without this and it cost a physical power
  // cycle: the spike serves no /api/update, so once it is running there is no
  // way back to the real firmware without touching the hardware.
  //
  // A restart route — NOT an update route — is the right primitive here. This
  // image is permanently ESP_OTA_IMG_PENDING_VERIFY, so esp_restart() makes the
  // bootloader roll back to the real firmware, and its /api/update takes over
  // from there. An update route on the spike would instead write into the slot
  // holding that firmware and destroy the rollback target.
  server.on("/restart", HTTP_POST, [](PsychicRequest*, PsychicResponse* res) -> esp_err_t {
    res->send(200, "application/json", "{\"restarting\":true,\"rollback\":true}");
    vTaskDelay(pdMS_TO_TICKS(500)); // let the response flush
    esp_restart();
    return ESP_OK; // unreachable
  });

  esp_err_t err = server.start();
  if (err != ESP_OK) {
    report(8, State::Fail, std::string("start: ") + esp_err_to_name(err));
    return;
  }
  char note[64];
  snprintf(note, sizeof(note), "serving " IPSTR "/", IP2STR(&wifiIp));
  report(8, State::Pass, note);
}

// --------------------------------------------------------------- main ------

extern "C" void app_main() {
  ESP_LOGI(TAG, "idf6 spike starting - IDF %s", esp_get_idf_version());
  ESP_LOGI(TAG, "boot heap: %s", heapLine().c_str());

  checkNvsCreds();
  checkLittlefs();
  checkSbFs();      // depends on check 2's mount; safe, confined to /lfs/.sbfs_test*
  checkPanel();     // panel comes up here; the three results above appear now
  checkBandPush();
  checkTouch();
  checkWifi();
  checkTlsApi();
  checkTlsCdn();
  checkPsychic();

  report(9, State::Pass, heapLine());

  // Last, and deliberately so — see the note on checkRootMount.
  checkRootMount();
  ESP_LOGI(TAG, "main task stack high water: %u",
           (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  int passes = 0, fails = 0;
  for (auto& c : checks) {
    if (c.state == State::Pass) ++passes;
    if (c.state == State::Fail) ++fails;
  }
  ESP_LOGI(TAG, "=== %d pass, %d fail, %d other ===", passes, fails,
           CHECK_COUNT - passes - fails);

  // Idle forever, live-reporting touch so the pad can be poked by hand. NOTE
  // the absence of any esp_ota_mark_app_valid_cancel_rollback() call — see the
  // safety note at the top of this file.
  while (true) {
    if (touchChan) {
      uint32_t sm[TOUCH_SAMPLE_CFG_NUM] = {0};
      if (touch_channel_read_data(touchChan, TOUCH_CHAN_DATA_TYPE_SMOOTH, sm) == ESP_OK)
        ESP_LOGI(TAG, "touch smooth %u | %s", (unsigned)sm[0], heapLine().c_str());
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
