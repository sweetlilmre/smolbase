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

// Free heap in bytes. MALLOC_CAP_INTERNAL deliberately, to match exactly what
// ESP.getFreeHeap() reported before this header existed:
//
//   Arduino: heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
//   esp_get_free_heap_size(): MALLOC_CAP_DEFAULT (8-bit-accessible only)
//
// The difference is ~52 KB on this chip — the IRAM-leftover heap region, which
// MALLOC_CAP_INTERNAL counts but which cannot serve a byte-buffer malloc. Using
// esp_get_free_heap_size() here silently rebased every heap figure the project
// has ever recorded, including the #119 "MPI allocation failure at ~48 KB free"
// threshold. Keep this comparable; use freeHeap8Bit() when the question is
// whether a byte buffer will actually fit.
inline uint32_t freeHeap() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

// Free heap that can serve ordinary byte buffers — the honest number for
// "will this TLS handshake fit". Lower than freeHeap() by the IRAM region.
inline uint32_t freeHeap8Bit() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }

// Largest single BYTE-BUFFER allocation still possible — what a TLS handshake
// actually needs, and the number that fails first under pressure (#119).
//
// MALLOC_CAP_8BIT, not INTERNAL, unlike freeHeap(): the two have opposite
// reasons. freeHeap() is INTERNAL to stay comparable with every heap figure
// this project has ever recorded. This one is asked "will an allocation fit",
// and the IRAM-leftover region INTERNAL counts cannot serve a byte buffer at
// all, so including it would answer a question nobody asked with a number
// nobody can spend. Nothing was reading this before it reached /api/status, so
// there is no historical series to keep faith with.
inline uint32_t largestFreeBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

// Lowest free heap seen since boot. Note the ruler: esp_get_minimum_free_heap_size()
// tracks MALLOC_CAP_DEFAULT, so this is the low-water mark of freeHeap8Bit(),
// NOT of freeHeap(). Compare it against free8Bit, never against free — that
// mismatch is a 52 KB phantom regression waiting to be discovered.
inline uint32_t minFreeHeap() { return esp_get_minimum_free_heap_size(); }

// Low-water free stack, in bytes, of the core-1 consumer loop task (ADR 0001).
// The only function here that is NOT inline: the task handle belongs to main.cpp,
// which owns the task, and this is the seam everything else already includes.
// Arduino sized that stack implicitly at 8 KB; now that main.cpp sizes it
// deliberately, /api/status reports this so the headroom is a measurement
// rather than a guess. 0 before the task exists.
uint32_t loopStackFree();

// Reboot (Arduino: ESP.restart()). Does not return.
[[noreturn]] inline void restart() {
  esp_restart();
  __builtin_unreachable();
}

} // namespace Platform
