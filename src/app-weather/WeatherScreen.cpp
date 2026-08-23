// Variant C layout (ticket #67 resolution — coordinates live there and in the
// prototype on the prototype/dashboard-layout branch). Rendering per ADR 0004:
// band painters compose into the static RGB565 scratch below (band-relative
// y), then push to the panel at the band's absolute y, clipped to the band's
// height. Full 16-bpp color — no RGB332 quantization — and pushes are already
// in the panel's wire format.
//
// Fonts are lv_font_conv bin blobs (scripts/build_assets.py) parsed once into
// static BFFfont instances — LovyanGFX's loadFont() slot holds only one
// runtime font, so we hold our own and switch with setFont(&face).
#include "WeatherScreen.h"
#include "../core/ConfigStore.h"
#include "../core/Net.h"
#include "../core/Platform.h"
#include "WeatherData.h"
#include "WeatherKeys.h"
#include "assets/wx_assets.h"
#include "smolbase_config.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace {

constexpr int W = 240;
// Element bands (absolute panel y). On-device tune, round 2 (#74): marquee
// dropped below-badge overlap with the clock; gauge row grew for the larger
// icons and labels. #104 grew the marquee to an 18 px face in a 24 px band
// and nudged the clock down to y=102 (Teko-96 ink is 60 px) to keep the gap.
constexpr int ICON_X = 8, ICON_Y = 8;
constexpr int CITY_Y = 14;
constexpr int BADGE_Y = 44, BADGE_H = 24;
constexpr int TOP_H = 72; // icon + city + badge band: rows 0..72
constexpr int MARQ_Y = 72, MARQ_H = 24;   // 24 px band for the 18 px face (#104)
constexpr int CLOCK_Y = 102, CLOCK_H = 64; // digit ink is 60 px; 4 px slack
constexpr int SEC_X = 196, SEC_REL_Y = 22; // seconds live inside the clock band
constexpr int DATE_Y = 180, DATE_H = 18;
constexpr int GAUGE_Y = 200, GAUGE_H = 34; // up from the bottom bezel (round 3)

// 24-bit RGB888 palette constants (see col() below for how LovyanGFX reads them).
constexpr uint32_t COL_AMBER = 0xfeba00, COL_CYAN = 0x99ffff, COL_GREEN = 0x99ff1f;
constexpr uint32_t COL_WDAY = 0x87cefa, COL_BADGE_BG = 0x2196f3, COL_TEMPBAR = 0xe53935;
constexpr uint32_t COL_WHITE = 0xffffff, COL_BLACK = 0x000000;
constexpr uint32_t COL_OVERLAY_BG = 0x001133;

constexpr uint32_t FRAME_MS = 33;     // marquee timestep: 1 px per 33 ms ≈ 30 px/s
constexpr uint32_t OVERLAY_MS = 5000; // identity overlay display duration
const char* const WDAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// LovyanGFX resolves integer color arguments by TYPE: 32-bit ints are RGB888
// on every target — the 16-bpp scratch and panel alike. This identity wrapper
// exists to make that contract visible at each call site.
constexpr uint32_t col(uint32_t rgb888) { return rgb888; }

// The band scratch (ADR 0004): one clock band tall — the tallest indivisible
// band (Teko-96 ink is 60 px). Static .bss for the same TLS-contiguity reason
// the core framebuffer was; 240 × 64 × 2 B = 30.7 KB, vs the 57.6 KB core
// buffer this env no longer compiles in.
constexpr int SCRATCH_H = CLOCK_H;
uint8_t scratchData[W * SCRATCH_H * 2];
lgfx::LGFX_Sprite scratch;

// Push the scratch's top `h` rows to the panel at band y. The panel clip
// makes pushSprite transfer only those rows.
void pushBand(lgfx::LovyanGFX& gfx, int y, int h) {
  gfx.setClipRect(0, y, W, h);
  scratch.pushSprite(&gfx, 0, y);
  gfx.clearClipRect();
}

// #RRGGBB → 0xRRGGBB (settings store the picker string; bad input = default).
uint32_t hexRgb(const std::string& s, uint32_t def) {
  if (s.length() != 7 || s[0] != '#') return def;
  char* end;
  uint32_t v = strtoul(s.c_str() + 1, &end, 16);
  return (end == s.c_str() + 7) ? v : def;
}

// One BFFfont per face, fed once from the flash arrays.
struct Face {
  lgfx::BFFfont font;
  lgfx::PointerWrapper data;
  bool load(const uint8_t* blob) {
    data.set(blob);
    return font.loadFont(&data);
  }
};
Face fClock, fSec, fCity, fBadge, fText, fMarq;

void drawIcon(const WxIcon& ic, int x, int y) {
  scratch.pushImage(x, y, ic.w, ic.h, (const void*)ic.data, 0u, lgfx::palette_4bit,
                    (const lgfx::rgb565_t*)ic.palette);
}

const WxIcon& conditionIcon(uint8_t code) {
  for (size_t i = 0; i < WX_ICON_COUNT; ++i)
    if (WX_ICON_CODES[i] == code) return WX_ICONS[i];
  return WX_ICONS[0]; // unknown → clear (SmolTV-Pro's fallback)
}

// Display formatting (SmolTV-Pro's exact output constants). Unit prefs are
// passed in from the cached bundle — no ConfigStore read at render time.
// Arduino's String(float, decimals) constructor has no std::string equivalent,
// so these are snprintf rather than a type swap. "%.2f" reproduces
// String(x, 2) exactly; the parity constants are unchanged.
std::string fmtTemp(float c, const WeatherScreen::Units& u) {
  char b[16];
  if (u.temp == "F") snprintf(b, sizeof(b), "%d" "\xC2\xB0" "F", (int)lroundf(c * 1.8f + 32));
  else snprintf(b, sizeof(b), "%d" "\xC2\xB0" "C", (int)lroundf(c));
  return b;
}
std::string fmtWind(float ms, const WeatherScreen::Units& u) {
  char b[24];
  if (u.wind == "kmh") snprintf(b, sizeof(b), "%.2f km/h", ms * 3.6f);
  else if (u.wind == "mph") snprintf(b, sizeof(b), "%.2f mile/h", ms * 2.2367f); // parity constant
  else snprintf(b, sizeof(b), "%.2f m/s", ms);
  return b;
}
std::string fmtPress(int hpa, const WeatherScreen::Units& u) {
  char b[20];
  if (u.press == "kpa") snprintf(b, sizeof(b), "%d kPa", hpa / 10);
  else if (u.press == "mmhg") snprintf(b, sizeof(b), "%d mmHg", (int)lroundf(hpa * 0.75f)); // parity constant
  else if (u.press == "inhg") snprintf(b, sizeof(b), "%d inHg", (int)lroundf(hpa * 0.0295300425f));
  else snprintf(b, sizeof(b), "%d hPa", hpa);
  return b;
}

} // namespace

void WeatherScreen::begin() {
  // A face that fails to parse stays unloaded and its text draws in the
  // fallback font — visible, not fatal. The BFF smoke test belongs to #74.
  fClock.load(WX_CLOCK96);
  fSec.load(WX_SEC40);
  fCity.load(WX_CITY22);
  fBadge.load(WX_BADGE16);
  fText.load(WX_TEXT16);
  fMarq.load(WX_MARQ18);
  scratch.setColorDepth(16);
  scratch.setBuffer(scratchData, W, SCRATCH_H, 16); // static — no heap sprite
  loadSettings();
}

void WeatherScreen::loadSettings() {
  // Numeric color fallbacks are DERIVED from the WeatherKeys.h registration
  // strings (#98): hexRgb on a well-formed constant can't fail, so the two
  // representations can't drift.
  colHour = hexRgb(ConfigStore::getString(WxKeys::COL_HOUR, WxKeys::DEF_COL_HOUR),
                   hexRgb(WxKeys::DEF_COL_HOUR, 0));
  colMin = hexRgb(ConfigStore::getString(WxKeys::COL_MIN, WxKeys::DEF_COL_MIN),
                  hexRgb(WxKeys::DEF_COL_MIN, 0));
  colSec = hexRgb(ConfigStore::getString(WxKeys::COL_SEC, WxKeys::DEF_COL_SEC),
                  hexRgb(WxKeys::DEF_COL_SEC, 0));
  h24 = ConfigStore::getBool(WxKeys::H24);
  dateFmt = ConfigStore::getString(WxKeys::DATE_FMT);
  if (!dateFmt.length()) dateFmt = WxKeys::DEF_DATE_FMT;
  nickname = ConfigStore::getString(WxKeys::NICKNAME);
  units.temp = ConfigStore::getString(WxKeys::UNIT_TEMP, WxKeys::DEF_UNIT_TEMP);
  units.wind = ConfigStore::getString(WxKeys::UNIT_WIND, WxKeys::DEF_UNIT_WIND);
  units.press = ConfigStore::getString(WxKeys::UNIT_PRESS, WxKeys::DEF_UNIT_PRESS);
  weatherDirty = true; // colours/units/format re-render from cached data (#68)
  lastSec = lastDay = -1;
}

void WeatherScreen::onEnter(lgfx::LGFX_Device& gfx) {
  gfx.setBaseColor(col(COL_BLACK));
  gfx.fillScreen(col(COL_BLACK));
  weatherDirty = true;
  lastSec = lastDay = -1;
  lastScrollMs = Platform::millis();
}

void WeatherScreen::onTap() { WeatherData::forceRefresh(); }

void WeatherScreen::onLongPress() {
  overlayUntilMs = Platform::millis() + OVERLAY_MS;
  overlayDirty = true;
}

void WeatherScreen::tick(lgfx::LGFX_Device& gfx) {
  if (parked) return;
  uint32_t now = Platform::millis();

  if (weatherDirty) {
    weatherDirty = false;
    drawWeather(gfx);
  }

  // Overlay request and expiry both just force the clock band's redraw —
  // it owns those rows and repaints whole (ADR 0004), so nothing to erase.
  if (overlayDirty) {
    overlayDirty = false;
    lastSec = -1;
  } else if (overlayUntilMs && now >= overlayUntilMs) {
    overlayUntilMs = 0;
    lastSec = -1;
  }

  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  if (tm.tm_sec != lastSec) {
    lastSec = tm.tm_sec;
    drawClockBand(gfx, tm);
  }
  if (tm.tm_mday != lastDay) {
    lastDay = tm.tm_mday;
    drawDate(gfx, tm);
  }

  // Marquee: time-based scroll, 1 px per 33 ms ≈ 30 px/s — the average holds
  // even if a pass runs long. Paused while the identity overlay is up.
  if (overlayUntilMs) {
    lastScrollMs = now; // no catch-up jump when the overlay expires
  } else if (now - lastScrollMs >= FRAME_MS) {
    int px = (int)((now - lastScrollMs) / FRAME_MS);
    lastScrollMs += (uint32_t)px * FRAME_MS;
    if (px > W) px = W; // stall (OTA pause etc.): don't spin the wrap loop
    drawMarquee(gfx, px);
  }
}

// ---- band painters -------------------------------------------------------------

void WeatherScreen::drawWeather(lgfx::LovyanGFX& gfx) {
  const WeatherData::Reading& r = cachedReading;

  std::string city = nickname.length() ? nickname : std::string(r.city);
  if (city.empty()) city = ConfigStore::getString(WxKeys::CITY, WxKeys::DEF_CITY);

  // Top band, 72 rows > the 64-row scratch: compose twice, content shifted up
  // by the pass offset, and push each slice clipped (ADR 0004). Rasterizing
  // the band twice happens only per fetch/settings change — free.
  for (int yOff = 0; yOff < TOP_H; yOff += SCRATCH_H) {
    scratch.fillScreen(col(COL_BLACK));

    if (r.valid) drawIcon(conditionIcon(r.iconCode), ICON_X, ICON_Y - yOff);

    // City centered, country code inline in amber.
    scratch.setTextDatum(lgfx::top_left);
    scratch.setFont(&fCity.font);
    // LovyanGFX takes const char* (or Arduino String), never std::string —
    // .c_str() at the draw boundary is the permanent pattern here.
    int cw = scratch.textWidth(city.c_str());
    scratch.setFont(&fBadge.font);
    int ccw = r.country[0] ? scratch.textWidth(r.country) + 6 : 0;
    int x0 = (W - cw - ccw) / 2;
    scratch.setFont(&fCity.font);
    scratch.setTextColor(col(COL_WHITE), col(COL_BLACK));
    scratch.drawString(city.c_str(), x0, CITY_Y - yOff);
    if (ccw) {
      scratch.setFont(&fBadge.font);
      scratch.setTextColor(col(COL_AMBER), col(COL_BLACK));
      scratch.drawString(r.country, x0 + cw + 6, CITY_Y + 7 - yOff);
    }

    // Condition badge, centered; width grows for long OWM mains ("Thunderstorm").
    const char* cond = r.valid ? r.condition : "--";
    scratch.setFont(&fBadge.font);
    int bw = scratch.textWidth(cond) + 14;
    if (bw < 72) bw = 72;
    scratch.fillRoundRect((W - bw) / 2, BADGE_Y - yOff, bw, BADGE_H, 4, col(COL_BADGE_BG));
    scratch.setTextDatum(lgfx::middle_center);
    scratch.setTextColor(col(COL_WHITE), col(COL_BADGE_BG));
    scratch.drawString(cond, W / 2, BADGE_Y + BADGE_H / 2 - yOff);
    scratch.setTextDatum(lgfx::top_left);

    int h = TOP_H - yOff < SCRATCH_H ? TOP_H - yOff : SCRATCH_H;
    pushBand(gfx, yOff, h);
  }

  // Gauge band: temp left, humidity right (band-relative y). x layout (round
  // 3): temp icon in from the bezel; slimmer bars so the temp label clears
  // the humidity icon and the % label clears the right edge.
  scratch.fillScreen(col(COL_BLACK));
  drawIcon(WX_GAUGE_TEMP, 10, 2);
  drawIcon(WX_GAUGE_HUMI, 130, 5);
  auto bar = [&](int x, float frac, uint32_t c) {
    scratch.drawRect(x, 9, 50, 14, col(COL_WHITE));
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    scratch.fillRect(x + 2, 11, (int)(46 * frac), 10, col(c));
  };
  bar(32, (r.tempC + 50.0f) / 100.0f, COL_TEMPBAR); // SmolTV-Pro's -50..50 range
  bar(152, r.humidity / 100.0f, COL_BADGE_BG);
  scratch.setFont(&fText.font);
  scratch.setTextColor(col(COL_WHITE), col(COL_BLACK));
  scratch.drawString(r.valid ? fmtTemp(r.tempC, units).c_str() : "--", 88, 11);
  scratch.drawString(r.valid ? (std::to_string(r.humidity) + "%").c_str() : "--", 206, 11);
  pushBand(gfx, GAUGE_Y, GAUGE_H);
}

void WeatherScreen::drawMarquee(lgfx::LovyanGFX& gfx, int scrollPx) {
  scratch.fillRect(0, 0, W, MARQ_H, col(COL_BLACK));
  const WeatherData::Reading& r = cachedReading;
  if (!r.valid) { // no fetch yet: a scroll of zeros would read as data
    pushBand(gfx, MARQ_Y, MARQ_H);
    return;
  }
  struct Seg {
    std::string text;
    uint32_t color;
  };
  const Seg segs[] = {
      {"Lowest ", COL_WHITE},       {fmtTemp(r.tempMinC, units), COL_CYAN},
      {", Highest ", COL_WHITE},    {fmtTemp(r.tempMaxC, units), COL_AMBER},
      {", Feels like ", COL_WHITE}, {fmtTemp(r.feelsC, units), COL_AMBER},
      {", Wind speed ", COL_WHITE}, {fmtWind(r.windMs, units), COL_GREEN},
      {", ATM ", COL_WHITE},        {fmtPress(r.pressureHpa, units), COL_AMBER},
      {"      ", COL_WHITE},
  };
  scratch.setFont(&fMarq.font);
  scratch.setTextDatum(lgfx::top_left);
  int lineW = 0;
  for (const Seg& s : segs) lineW += scratch.textWidth(s.text.c_str());
  if (lineW <= 0) return;
  marqX -= scrollPx;
  while (marqX <= -lineW) marqX += lineW;
  // Consecutive copies cover the band; the scratch clips glyphs at the edges.
  for (int x = marqX; x < W; x += lineW) {
    int tx = x;
    for (const Seg& s : segs) {
      scratch.setTextColor(col(s.color), col(COL_BLACK));
      scratch.drawString(s.text.c_str(), tx, 2);
      tx += scratch.textWidth(s.text.c_str());
    }
  }
  pushBand(gfx, MARQ_Y, MARQ_H);
}

// The whole band — hh:mm + seconds, or the identity overlay — redraws every
// second (ADR 0004): a few ms of rasterization at 1 Hz buys away the seconds
// special case and all overlay erase logic.
void WeatherScreen::drawClockBand(lgfx::LovyanGFX& gfx, const struct tm& tm) {
  if (overlayUntilMs) {
    scratch.fillScreen(col(COL_OVERLAY_BG));
    scratch.setFont(&fText.font);
    scratch.setTextDatum(lgfx::middle_center);
    scratch.setTextColor(col(COL_WHITE), col(COL_OVERLAY_BG));
    scratch.drawString(Net::deviceName().c_str(), W / 2, 14);
    scratch.setTextColor(col(COL_CYAN), col(COL_OVERLAY_BG));
    scratch.drawString(Net::ip().c_str(), W / 2, 32);
    scratch.setTextColor(col(COL_WHITE), col(COL_OVERLAY_BG));
    scratch.drawString(SMOLBASE_FW_VERSION, W / 2, 51);
    scratch.setTextDatum(lgfx::top_left);
    pushBand(gfx, CLOCK_Y, CLOCK_H);
    return;
  }

  int hour = tm.tm_hour;
  if (!h24) {
    hour = hour % 12;
    if (hour == 0) hour = 12; // 12 h mode: no AM/PM, no leading zero (parity)
  }
  char hs[6], ms[3], ss[3];
  snprintf(hs, sizeof(hs), h24 ? "%02d:" : "%d:", hour);
  snprintf(ms, sizeof(ms), "%02d", tm.tm_min);
  snprintf(ss, sizeof(ss), "%02d", tm.tm_sec);

  scratch.fillScreen(col(COL_BLACK));
  scratch.setFont(&fClock.font);
  int wh = scratch.textWidth(hs), wm = scratch.textWidth(ms);
  int x0 = (W - wh - wm) / 2;
  scratch.setTextDatum(lgfx::top_left);
  scratch.setTextColor(col(colHour), col(COL_BLACK)); // colon inherits hour color (#67)
  scratch.drawString(hs, x0, 0);
  scratch.setTextColor(col(colMin), col(COL_BLACK));
  scratch.drawString(ms, x0 + wh, 0);
  scratch.setFont(&fSec.font);
  scratch.setTextColor(col(colSec), col(COL_BLACK));
  scratch.drawString(ss, SEC_X, SEC_REL_Y);
  pushBand(gfx, CLOCK_Y, CLOCK_H);
}

void WeatherScreen::drawDate(lgfx::LovyanGFX& gfx, const struct tm& tm) {
  char buf[24];
  strftime(buf, sizeof(buf), dateFmt.c_str(), &tm);
  const char* wd = WDAY[tm.tm_wday];

  scratch.fillRect(0, 0, W, DATE_H, col(COL_BLACK));
  scratch.setFont(&fText.font);
  int dw = scratch.textWidth(buf), ww = scratch.textWidth(wd);
  int x0 = (W - dw - ww - 6) / 2;
  scratch.setTextDatum(lgfx::top_left);
  scratch.setTextColor(col(COL_WHITE), col(COL_BLACK));
  scratch.drawString(buf, x0, 0);
  scratch.setTextColor(col(COL_WDAY), col(COL_BLACK));
  scratch.drawString(wd, x0 + dw + 6, 0);
  pushBand(gfx, DATE_Y, DATE_H);
}
