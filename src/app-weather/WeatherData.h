// Weather data layer (#70): providers, fallback, geocoder — the API the
// dashboard Screen consumes (display formatting lives with the Screen, #88).
// Functional parity with SmolTV-Pro's logic (map #63), reimplemented on
// smolbase idioms.
//
// Threading (ADR 0001): fetches run on a consumer-spawned FreeRTOS task —
// TLS handshakes take seconds, far past the loop budget. The main loop stays
// the only consumer surface: loop() schedules fetches and promotes finished
// readings under a spinlock; takeChanged() is main-loop-only.
//
// Transport: both providers over HTTPS via the stock IDF CA bundle (#82).
// Define SMOLBASE_WEATHER_HTTP=1 to drop everything to plain HTTP — the
// researched last-resort switch (see the scheme fork in WeatherData.cpp).
#pragma once
#include <cstdint>
#include <functional>

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

// begin() spawns the fetch task; call from App::setup (after registration).
// The hooks bracket every fetch cycle for RAM choreography (#74): the TLS
// handshake peaks at ~49 KB on this heap, so onFetchBegin fires on the
// loop() pass that arms the task — free what you can — and onFetchEnd on
// the pass that promotes the cycle's result (success or failure alike).
// WeatherData owns the fetch window; callers never re-derive scheduling
// state — the old fetchQueued()/fetchBusy() predicates missed the
// interval-driven fetch entirely (#94).
void begin(std::function<void()> onFetchBegin, std::function<void()> onFetchEnd);
void loop(); // every main-loop pass: schedule fetches, promote results

// One-shot: the newly promoted Reading, or nullptr when nothing new since
// the last call. The ONLY way to observe a reading (main-loop only), so a
// stale read outside the "new data" branch is unrepresentable (#97).
const Reading* takeChanged();

void forceRefresh();      // tap, or a fetch-relevant settings save
void onSettingsChanged(); // detects a city change: new geocode + refetch

// The debug surface lives in WeatherDebug.h (#99) — the one consumer
// allowed off the main loop, and the reason it is not declared here.

} // namespace WeatherData
