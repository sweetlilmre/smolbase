// Weather data layer (#70): providers, fallback, geocoder, units — the API
// the dashboard Screen consumes. Functional parity with SmolTV-Pro's logic
// (map #63), reimplemented on smolbase idioms.
//
// Threading (ADR 0001): fetches run on a consumer-spawned FreeRTOS task —
// TLS handshakes take seconds, far past the loop budget. The main loop stays
// the only consumer surface: loop() schedules fetches and promotes finished
// readings under a spinlock; reading()/changed() are main-loop-only.
//
// Transport: plain HTTP by default — the researched fallback position,
// adopted after HTTPS was exercised on-device and the pinned core's CA
// bundle failed X509 verification against both providers node-dependently
// (see the verdict comment in WeatherData.cpp). Define SMOLBASE_WEATHER_TLS=1
// to opt the keyed OWM request back into HTTPS via the built-in bundle.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace WeatherData {

// One fetched observation. Numbers stay METRIC — display conversion is the
// fmt* helpers' job, so a units save re-renders cached data with no refetch
// (#68 Q4). On the keyless path feels/humidity/pressure are 0 by contract.
struct Reading {
  bool valid = false; // false until the first successful fetch ever
  bool keyless = false;
  float tempC = 0, tempMinC = 0, tempMaxC = 0, feelsC = 0, windMs = 0;
  int humidity = 0, pressureHpa = 0;
  uint8_t iconCode = 1; // OWM icon-prefix code: 1,2,3,4,9,10,11,13,50
  char condition[24] = "";
  char city[32] = ""; // resolved name (nickname override is the Screen's call)
  char country[4] = "";
};

void begin(); // spawn the fetch task; call from App::setup (after registration)
void loop();  // every main-loop pass: schedule fetches, promote results

// Fetch lifecycle for RAM choreography (#74): the TLS handshake peaks at
// ~49 KB on this heap, so the app frees what it can for the duration.
// fetchQueued() is true on the pass that will arm the task; fetchBusy()
// stays true until the cycle's result is promoted.
bool fetchQueued();
bool fetchBusy();

const Reading& reading(); // main-loop only
bool changed();           // one-shot: true once after each promoted reading

void forceRefresh();      // tap, or a fetch-relevant settings save
void onSettingsChanged(); // detects a city change: new geocode + refetch

// Debug surface (#74 flight readiness): the current Reading plus the last
// fetch cycle's per-stage HTTP codes (0 = stage not run, -100 = begin/
// connect failed, -101 = 200-but-parse-failed). Never includes the key.
void debugJson(JsonDocument& out);

} // namespace WeatherData
