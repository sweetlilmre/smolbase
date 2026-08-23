// The weather App's status surface, reached through App::statusJson and landing
// in the "app" object of GET /api/status (#74, #99).
//
// json() is the ONE weather consumer allowed off the main loop: it runs on the
// httpd task (core 0), so it copies the shared fields under the fetch mux
// before serializing. Reports the current reading, where that reading's
// coordinates came from, and whether fetching is healthy — plus key PRESENCE,
// never the key (ADR 0003).
//
// It used to be /api/debug/weather, and it used to carry a 20-entry heap
// trajectory from the #74 TLS-OOM investigation. That array stopped recording
// after the first few fetch cycles by design, so it reported the first minutes
// after boot forever, on a heap ruler that disagreed with every other figure in
// the firmware by ~52 KB. The investigation it served is closed
// (docs/app-weather-memory.md); /api/status carries the heap centrally and live.
//
// Defined in WeatherData.cpp beside the internal state it reads; this header
// exists so WeatherData.h stays free of ArduinoJson and the status concern.
#pragma once
#include <ArduinoJson.h>

namespace WeatherStatus {

void json(JsonObject out); // core-0-callable

} // namespace WeatherStatus
