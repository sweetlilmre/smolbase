// Platform primitives: monotonic time, heap introspection, reset.
//
// Every function here is implemented over ESP-IDF APIs that exist under BOTH
// arduino-esp32 and native ESP-IDF, so this header needs no #ifdef and will not
// change when the framework does (wayfinder: the IDF 6 migration, phase 2).
// That is the whole point — it is the seam that lets 47 Arduino call sites move
// now, on a build that can still be flashed and reverted.
//
// Not a compatibility shim for Arduino's API: the names are ours, the
// implementations are IDF's. See docs/research/esp-idf-6-migration.md for why
// the larger Arduino objects (String, WiFi, Update, Preferences) are
// deliberately NOT wrapped this way.
//
// Wall-clock time lives in Clock; this is monotonic uptime only.
#pragma once

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdint>

namespace Platform {

// Milliseconds since boot. Wraps after ~49 days, exactly like Arduino's
// millis(), so existing `now - then >= interval` comparisons stay correct.
inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

// Microseconds since boot. Wraps after ~71 minutes.
inline uint32_t micros() { return (uint32_t)esp_timer_get_time(); }

// Yields to the scheduler for at least `ms`. Unlike Arduino's delay() this is
// always a real task block — never a busy-wait — so it feeds the watchdog.
// A 0 ms sleep still yields one tick.
inline void delayMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }

// Free heap in bytes (Arduino: ESP.getFreeHeap()).
inline uint32_t freeHeap() { return esp_get_free_heap_size(); }

// Largest single allocation still possible (Arduino: ESP.getMaxAllocHeap()).
// This, not freeHeap(), is what a TLS handshake actually needs.
inline uint32_t largestFreeBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

// Lowest freeHeap() seen since boot.
inline uint32_t minFreeHeap() { return esp_get_minimum_free_heap_size(); }

// Reboot (Arduino: ESP.restart()). Does not return.
[[noreturn]] inline void restart() {
  esp_restart();
  __builtin_unreachable();
}

} // namespace Platform
