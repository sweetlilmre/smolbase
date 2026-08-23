// Single capacitive pad on T9/GPIO32, via the unified touch-sensor driver
// (driver/touch_sens.h) rather than Arduino's touchRead().
//
// Two driver facts shape this file:
//
// 1. READ SMOOTH, NOT RAW. hw_ver1/touch_version_specific.c guards reads with
//    `type == SMOOTH && filter != NULL` where it means `type != SMOOTH ||
//    filter != NULL`, so a RAW read returns ESP_ERR_INVALID_STATE
//    unconditionally. Present in both IDF 5.5.5 and 6.0.2; reported and fixed
//    upstream (espressif/esp-idf#18811) after the 6.0.2 tag. Configuring the
//    software filter and reading SMOOTH works on every version, buggy or
//    fixed, so that is what we do rather than gating on an IDF version.
//
// 2. V1 VALUES FALL WHEN TOUCHED, and their scale is nothing like touchRead()'s.
//    Arduino baselines sat in the hundreds; this driver reports ~1600 on the
//    same pad. So the press margin is a PERCENTAGE of the measured baseline,
//    not an absolute count — the driver's own test app does the same
//    (abs_active_thresh = benchmark * (1 - coeff)). Porting the old absolute
//    margin across would have left it silently far too small to ever trigger.
//
// The driver's own active-threshold callbacks are unused: thresholding happens
// here so the debounce / tap / long-press logic stays exactly as it was.
#include "Touch.h"
#include "Display.h"
#include "Platform.h"
#include "smolbase_config.h"

#include <driver/touch_sens.h>

namespace Touch {

static touch_sensor_handle_t sens = nullptr;
static touch_channel_handle_t chan = nullptr;

static uint32_t baseline = 0;  // untouched reading measured at boot; 0 = pad unusable
static uint32_t threshold = 0; // pressed when the reading falls below this
static uint32_t lastRead = 0;  // most recent sample, for calibration observability

// Raw (undebounced) pad state and when it last changed.
static bool rawState = false;
static uint32_t rawSince = 0;

// Debounced press state and long-press bookkeeping.
static bool down = false;
static uint32_t downAt = 0;
static bool longFired = false;

// False when the driver has nothing for us (filter not yet warm, or the
// controller failed to start). Callers must not read that as "not pressed" —
// it is "no reading", which is a different thing.
static bool readPad(uint32_t& out) {
  if (!chan) return false;
  uint32_t v[TOUCH_SAMPLE_CFG_NUM] = {0};
  if (touch_channel_read_data(chan, TOUCH_CHAN_DATA_TYPE_SMOOTH, v) != ESP_OK) return false;
  out = v[0];
  return v[0] != 0;
}

uint32_t padBaseline() { return baseline; }
uint32_t padThreshold() { return threshold; }
uint32_t padLast() { return lastRead; }

void begin() {
  touch_sensor_sample_config_t sample[TOUCH_SAMPLE_CFG_NUM] = {
      TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(SMOLBASE_TOUCH_CHARGE_MS, TOUCH_VOLT_LIM_L_0V5,
                                            TOUCH_VOLT_LIM_H_1V7),
  };
  touch_sensor_config_t cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, sample);
  if (touch_sensor_new_controller(&cfg, &sens) != ESP_OK) return;

  // Do NOT zero-initialise this: charge_speed 0 is a real enum value (the
  // slowest), not "unset", and the pad then never charges — every reading comes
  // back 0. Values are the driver's own V1 test app.
  touch_channel_config_t chanCfg = {};
  chanCfg.abs_active_thresh[0] = 1; // unused; we threshold in loop()
  chanCfg.charge_speed = TOUCH_CHARGE_SPEED_7;
  chanCfg.init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT;
  chanCfg.group = TOUCH_CHAN_TRIG_GROUP_BOTH;
  if (touch_sensor_new_channel(sens, SMOLBASE_TOUCH_CHANNEL, &chanCfg, &chan) != ESP_OK) return;

  // Mandatory, not optional: without a filter every read is rejected (header
  // note 1). A null data_filter_fn installs the driver's default.
  touch_sensor_filter_config_t filter = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
  if (touch_sensor_config_filter(sens, &filter) != ESP_OK) return;

  if (touch_sensor_enable(sens) != ESP_OK) return;
  if (touch_sensor_start_continuous_scanning(sens) != ESP_OK) return;

  // The software filter runs on a 10 ms timer; let it produce a value before
  // sampling, or every calibration read is zero.
  Platform::delayMs(SMOLBASE_TOUCH_SETTLE_MS);

  // Average several untouched readings — a single sample is noisy on the ESP32
  // touch peripheral and would make the threshold a coin toss.
  uint32_t sum = 0;
  int got = 0;
  for (int i = 0; i < SMOLBASE_TOUCH_CAL_SAMPLES; ++i) {
    uint32_t v = 0;
    if (readPad(v)) {
      sum += v;
      ++got;
    }
    Platform::delayMs(SMOLBASE_TOUCH_CAL_GAP_MS);
  }
  if (!got) return; // baseline stays 0, so loop() does nothing at all

  baseline = sum / (uint32_t)got;
  // Percentage margin, because the value scale is driver-specific (header note 2).
  threshold = baseline - (baseline * SMOLBASE_TOUCH_DELTA_PCT) / 100;
}

void loop() {
  if (!threshold) return; // calibration failed: no phantom taps from a dead pad

  uint32_t v = 0;
  if (!readPad(v)) return; // no reading is not the same as "not pressed"
  lastRead = v;

  uint32_t now = Platform::millis();
  bool raw = v < threshold; // V1: the value FALLS when touched

  // Track how long the raw state has been stable.
  if (raw != rawState) {
    rawState = raw;
    rawSince = now;
  }

  // Accept an edge only once the raw state has held for the debounce window,
  // so a single noisy sample can't fire a phantom tap or drop a hold.
  if (rawState != down && now - rawSince >= SMOLBASE_TOUCH_DEBOUNCE_MS) {
    down = rawState;
    if (down) { // press edge: timed from first stable contact
      downAt = rawSince;
      longFired = false;
    } else if (!longFired) { // release edge: short hold = tap
      Screen* s = Display::active();
      if (s) s->onTap();
    }
  }

  // Long-press fires once while still held; the release then emits nothing.
  if (down && !longFired && now - downAt >= SMOLBASE_TOUCH_LONGPRESS_MS) {
    longFired = true;
    Screen* s = Display::active();
    if (s) s->onLongPress();
  }
}

} // namespace Touch
