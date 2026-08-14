// Single source (#98) for the weather app's Config Store keys and machine
// defaults, cited by registration (WeatherApp), reads (WeatherScreen,
// WeatherData), and fallbacks alike — the strings can no longer drift.
// Human-facing labels and choice catalogs stay with the registration in
// WeatherApp.cpp. The geocode-cache keys (unregistered machine state) stay
// private to WeatherData.cpp — no other TU may touch them.
//
// Color defaults exist here as their registration strings ONLY; numeric
// forms are derived at load (hexRgb on the constant — can't fail), never
// hand-maintained.
#pragma once

namespace WxKeys {

constexpr const char* CITY = "city";
constexpr const char* DEF_CITY = "Durban";

constexpr const char* NICKNAME = "nickname"; // default: empty

constexpr const char* COL_HOUR = "col_hour";
constexpr const char* DEF_COL_HOUR = "#FFFFFF";
constexpr const char* COL_MIN = "col_min";
constexpr const char* DEF_COL_MIN = "#FF5A00";
constexpr const char* COL_SEC = "col_sec";
constexpr const char* DEF_COL_SEC = "#FF5900";

constexpr const char* H24 = "h24";
constexpr bool DEF_H24 = true;

constexpr const char* DATE_FMT = "date_fmt";
constexpr const char* DEF_DATE_FMT = "%d/%m/%Y";

constexpr const char* UNIT_TEMP = "unit_temp";
constexpr const char* DEF_UNIT_TEMP = "C";
constexpr const char* UNIT_WIND = "unit_wind";
constexpr const char* DEF_UNIT_WIND = "ms";
constexpr const char* UNIT_PRESS = "unit_press";
constexpr const char* DEF_UNIT_PRESS = "hpa";

constexpr const char* INTERVAL = "wx_interval";
constexpr int DEF_INTERVAL_MIN = 20;

constexpr const char* OWM_KEY = "owm_api_key"; // Secret Store name (ADR 0003)

} // namespace WxKeys
