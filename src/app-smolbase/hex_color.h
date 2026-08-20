// Shared by the stock screens: "#RRGGBB" -> 24-bit RGB; anything malformed
// falls back. The settings store keeps colors as the same string an
// <input type="color"> speaks (registerColor, ticket #58), so the web side
// needs no conversion — parsing happens once per repaint, here.
#pragma once
#include <Arduino.h>

inline uint32_t hexRgb(const String& s, uint32_t fallback) {
  if (s.length() == 7 && s[0] == '#') {
    char* end;
    long v = strtol(s.c_str() + 1, &end, 16);
    if (*end == '\0') return (uint32_t)v;
  }
  return fallback;
}
