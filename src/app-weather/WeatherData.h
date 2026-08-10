// Weather data layer (#70): providers, fallback, geocoder, units — the API
// the dashboard Screen consumes. Functional parity with SmolTV-Pro's logic
// (map #63), reimplemented on smolbase idioms.
//
// Threading (ADR 0001): fetches run on a consumer-spawned FreeRTOS task —
// TLS handshakes take seconds, far past the loop budget. The main loop stays
// the only consumer surface: loop() schedules fetches and promotes finished
// readings under a spinlock; reading()/changed() are main-loop-only.
//
// Transport: HTTPS via NetworkClientSecure + the built-in ESP x509 bundle
// (docs/research/https-weather-fetches.md). Define SMOLBASE_WEATHER_HTTP=1
// to drop to plain HTTP — the researched fallback position, not a rewrite.
#pragma once
#include <Arduino.h>

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

const Reading& reading(); // main-loop only
bool changed();           // one-shot: true once after each promoted reading

void forceRefresh();      // tap, or a fetch-relevant settings save
void onSettingsChanged(); // detects a city change: new geocode + refetch

// Display formatting per the unit settings (#68 catalog values), applied to
// the cached metric reading. SmolTV-Pro's exact display constants.
String fmtTemp(float c);      // "22°C" / "72°F" (integer)
String fmtWind(float ms);     // "3.60 m/s" / "12.96 km/h" / "8.05 mile/h"
String fmtPress(int hpa);     // "1013 hPa" / "101 kPa" / "760 mmHg" / "29 inHg"

} // namespace WeatherData
