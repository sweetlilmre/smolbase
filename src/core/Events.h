// System event bus. Events may be posted from any core/task; they are drained by the
// main loop (core 1), which is what keeps all consumer code single-threaded (ADR 0001).
#pragma once
#include <cstdint>

enum class SysEvent : uint8_t {
  NetworkUp,     // STA connected, IP acquired
  NetworkDown,   // STA lost (auto-reconnect is already running)
  ApModeEntered, // provisioning AP is up
  TimeSynced,    // SNTP got real time
  OtaStarting,   // stop drawing/allocating; flash write imminent
  SettingsChanged,
};

namespace Events {
void begin();
bool post(SysEvent e); // safe from any task/ISR-adjacent context
void drain(void (*handler)(SysEvent)); // main loop only
} // namespace Events
