// Variant C layout (ticket #67 resolution — coordinates live there and in the
// prototype on the prototype/dashboard-layout branch). Drawing goes DIRECT to
// the panel — no full framebuffer: every element owns a black rectangle it
// clears and repaints on its own cadence (dirty-flag pattern, ticket #6), and
// AA text blends toward the drawn background per the asset research. The
// marquee is the exception: an 8-bpp sprite holding the full line, tiled at
// a scrolling offset each frame (LovyanGFX clips to the panel, so only the
// visible band is transmitted) — and surrendered to the TLS handshake for
// the duration of each fetch (see suspendMarquee).
//
// Fonts are lv_font_conv bin blobs (scripts/build_assets.py) parsed once into
// static BFFfont instances — LovyanGFX's loadFont() slot holds only one
// runtime font, so we hold our own and switch with setFont(&face).
#include "WeatherScreen.h"
#include "../core/ConfigStore.h"
#include "../core/Net.h"
#include "WeatherData.h"
#include "assets/wx_assets.h"
#include "smolbase_config.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace {

constexpr int W = 240;
// Element bands (each element clears its own). On-device tune, round 2 (#74):
// marquee dropped below-badge overlap with the clock, so the clock moved to
// y=98 (its Teko-96 digits ink 60 px tall) and the marquee got a 20 px band
// for the 15 px face; gauge row grew for the larger icons and labels.
constexpr int ICON_X = 8, ICON_Y = 8;
constexpr int CITY_Y = 14;
constexpr int BADGE_Y = 44, BADGE_H = 24;
constexpr int MARQ_Y = 72, MARQ_H = 20;
constexpr int CLOCK_Y = 98, CLOCK_H = 64; // digit ink is 60 px; 4 px slack
constexpr int SEC_X = 196, SEC_Y = 120;
constexpr int DATE_Y = 180, DATE_H = 18;
constexpr int GAUGE_Y = 200, GAUGE_H = 34; // up from the bottom bezel (round 3)
// The marquee sprite is allocated ONCE at begin(), full size, while the heap
// is unfragmented: a mid-session (re)alloc carves up the largest free block
// and TLS handshakes (2x ~16.7 KB record buffers) start failing — measured
// on-device. 8 bpp (RGB332) halves the cost; the line colors survive fine.
constexpr int MARQ_W_MAX = 672;

// 24-bit RGB888 palette constants (see col() below for how LovyanGFX reads them).
constexpr uint32_t COL_AMBER = 0xfeba00, COL_CYAN = 0x99ffff, COL_GREEN = 0x99ff1f;
constexpr uint32_t COL_WDAY = 0x87cefa, COL_BADGE_BG = 0x2196f3, COL_TEMPBAR = 0xe53935;
constexpr uint32_t COL_WHITE = 0xffffff, COL_BLACK = 0x000000;
constexpr uint32_t COL_OVERLAY_BG = 0x001133;

constexpr uint32_t OVERLAY_MS = 5000; // identity overlay display duration
const char* const WDAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// LovyanGFX resolves integer color arguments by TYPE: 32-bit ints are RGB888
// on every target — the 16-bpp panel and the 8-bpp screenshot sprite alike
// (uint16_t would mean RGB565, uint8_t RGB332). This identity wrapper exists
// to make that contract visible at each call site.
constexpr uint32_t col(uint32_t rgb888) { return rgb888; }

// #RRGGBB → 0xRRGGBB (settings store the picker string; bad input = default).
uint32_t hexRgb(const String& s, uint32_t def) {
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
Face fClock, fSec, fCity, fDate, fBadge, fText;

void drawIcon(lgfx::LovyanGFX& gfx, const WxIcon& ic, int x, int y) {
  gfx.pushImage(x, y, ic.w, ic.h, (const void*)ic.data, 0u, lgfx::palette_4bit,
                (const lgfx::rgb565_t*)ic.palette);
}

const WxIcon& conditionIcon(uint8_t code) {
  for (size_t i = 0; i < WX_ICON_COUNT; ++i)
    if (WX_ICON_CODES[i] == code) return WX_ICONS[i];
  return WX_ICONS[0]; // unknown → clear (SmolTV-Pro's fallback)
}

// Display formatting (SmolTV-Pro's exact output constants). Unit prefs are
// passed in from the cached bundle — no ConfigStore read at render time.
String fmtTemp(float c, const WeatherScreen::Units& u) {
  if (u.temp == "F") return String((int)lroundf(c * 1.8f + 32)) + "\xC2\xB0" "F";
  return String((int)lroundf(c)) + "\xC2\xB0" "C";
}
String fmtWind(float ms, const WeatherScreen::Units& u) {
  if (u.wind == "kmh") return String(ms * 3.6f, 2) + " km/h";
  if (u.wind == "mph") return String(ms * 2.2367f, 2) + " mile/h"; // parity constant
  return String(ms, 2) + " m/s";
}
String fmtPress(int hpa, const WeatherScreen::Units& u) {
  if (u.press == "kpa") return String(hpa / 10) + " kPa";
  if (u.press == "mmhg") return String((int)lroundf(hpa * 0.75f)) + " mmHg"; // parity constant
  if (u.press == "inhg") return String((int)lroundf(hpa * 0.0295300425f)) + " inHg";
  return String(hpa) + " hPa";
}

} // namespace

void WeatherScreen::begin() {
  // A face that fails to parse stays unloaded and its text draws in the
  // fallback font — visible, not fatal. The BFF smoke test belongs to #74.
  fClock.load(WX_CLOCK96);
  fSec.load(WX_SEC40);
  fCity.load(WX_CITY22);
  fDate.load(WX_DATE15);
  fBadge.load(WX_BADGE16);
  fText.load(WX_TEXT15);
  marq.setColorDepth(8);
  marq.createSprite(MARQ_W_MAX, MARQ_H); // once, pristine heap — see MARQ_W_MAX
  loadSettings();
}

void WeatherScreen::loadSettings() {
  colHour = hexRgb(ConfigStore::getString("col_hour"), 0xffffff);
  colMin = hexRgb(ConfigStore::getString("col_min"), 0xff5a00);
  colSec = hexRgb(ConfigStore::getString("col_sec"), 0xff5900);
  h24 = ConfigStore::getBool("h24");
  dateFmt = ConfigStore::getString("date_fmt");
  if (!dateFmt.length()) dateFmt = "%d/%m/%Y";
  nickname = ConfigStore::getString("nickname");
  units.temp = ConfigStore::getString("unit_temp", "C");
  units.wind = ConfigStore::getString("unit_wind", "ms");
  units.press = ConfigStore::getString("unit_press", "hpa");
  weatherDirty = true; // colours/units/format re-render from cached data (#68)
  lastMin = lastSec = lastDay = -1;
}

void WeatherScreen::onEnter(lgfx::LGFX_Device& gfx) {
  gfx.setBaseColor(col(COL_BLACK));
  gfx.fillScreen(col(COL_BLACK));
  weatherDirty = true;
  lastMin = lastSec = lastDay = -1;
}

void WeatherScreen::onTap() { WeatherData::forceRefresh(); }

void WeatherScreen::onLongPress() {
  overlayUntilMs = millis() + OVERLAY_MS;
  overlayDirty = true;
}

void WeatherScreen::drawIdentityOverlay(lgfx::LovyanGFX& gfx) {
  // Overlay sits in the clock band — drawClock() clears it on expiry.
  gfx.fillRect(0, CLOCK_Y, W, CLOCK_H, col(COL_OVERLAY_BG));
  gfx.setFont(&fDate.font);
  gfx.setTextDatum(lgfx::middle_center);
  gfx.setTextColor(col(COL_WHITE), col(COL_OVERLAY_BG));
  gfx.drawString(Net::deviceName(), W / 2, CLOCK_Y + 14);
  gfx.setTextColor(col(COL_CYAN), col(COL_OVERLAY_BG));
  gfx.drawString(Net::ip().toString(), W / 2, CLOCK_Y + 32);
  gfx.setTextColor(col(COL_WHITE), col(COL_OVERLAY_BG));
  gfx.drawString(SMOLBASE_FW_VERSION, W / 2, CLOCK_Y + 51);
  gfx.setTextDatum(lgfx::top_left);
}

// Both are idempotent, called every loop pass by the app (core 1, same as
// tick — no race with the scroll). Suspend leaves the last-pushed pixels
// frozen in the band; resume rebuilds the line from the cached reading.
void WeatherScreen::suspendMarquee() {
  if (marq.getBuffer()) {
    marq.deleteSprite();
    marqWidth = 0;
  }
}

void WeatherScreen::resumeMarquee() {
  if (!marq.getBuffer()) {
    marq.setColorDepth(8);
    if (marq.createSprite(MARQ_W_MAX, MARQ_H)) rebuildMarquee();
  }
}

void WeatherScreen::tick(lgfx::LGFX_Device& gfx) {
  if (parked) return;
  if (weatherDirty) {
    weatherDirty = false;
    drawWeather(gfx);
  }
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  if (tm.tm_min != lastMin) {
    lastMin = tm.tm_min;
    if (!overlayUntilMs) drawClock(gfx, tm);
  }
  if (tm.tm_sec != lastSec) {
    lastSec = tm.tm_sec;
    if (!overlayUntilMs) drawSeconds(gfx, tm.tm_sec);
  }
  if (tm.tm_mday != lastDay) {
    lastDay = tm.tm_mday;
    drawDate(gfx, tm);
  }
  // Identity overlay: draw once on request; suppress marquee for its duration;
  // force clock repaint when it expires (drawClock clears the CLOCK_Y band).
  uint32_t now = millis();
  if (overlayDirty) {
    overlayDirty = false;
    drawIdentityOverlay(gfx);
  } else if (overlayUntilMs && now >= overlayUntilMs) {
    overlayUntilMs = 0;
    lastMin = lastSec = -1; // trigger drawClock + drawSeconds to erase the overlay
  }
  // Marquee: ~30 Hz, 1 px/frame ≈ 30 px/s — paused while identity overlay is up.
  if (marqWidth > 0 && now - lastFrameMs >= 33 && !overlayUntilMs) {
    lastFrameMs = now;
    if (--marqX <= -marqWidth) marqX += marqWidth;
    // Consecutive copies cover the whole band; the panel clips the rest.
    for (int x = marqX; x < W; x += marqWidth) marq.pushSprite(&gfx, x, MARQ_Y);
  }
}

// ---- element painters --------------------------------------------------------

void WeatherScreen::drawWeather(lgfx::LovyanGFX& gfx) {
  const WeatherData::Reading& r = cachedReading;

  // Icon + city + badge share the top band; clear it wholesale.
  gfx.fillRect(0, 0, W, MARQ_Y, col(COL_BLACK));

  String city = nickname.length() ? nickname : String(r.city);
  if (!city.length()) city = ConfigStore::getString("city", "Durban");

  if (r.valid) drawIcon(gfx, conditionIcon(r.iconCode), ICON_X, ICON_Y);

  // City centered, country code inline in amber.
  gfx.setTextDatum(lgfx::top_left);
  gfx.setFont(&fCity.font);
  int cw = gfx.textWidth(city);
  gfx.setFont(&fBadge.font);
  int ccw = r.country[0] ? gfx.textWidth(r.country) + 6 : 0;
  int x0 = (W - cw - ccw) / 2;
  gfx.setFont(&fCity.font);
  gfx.setTextColor(col(COL_WHITE), col(COL_BLACK));
  gfx.drawString(city, x0, CITY_Y);
  if (ccw) {
    gfx.setFont(&fBadge.font);
    gfx.setTextColor(col(COL_AMBER), col(COL_BLACK));
    gfx.drawString(r.country, x0 + cw + 6, CITY_Y + 7);
  }

  // Condition badge, centered; width grows for long OWM mains ("Thunderstorm").
  const char* cond = r.valid ? r.condition : "--";
  gfx.setFont(&fBadge.font);
  int bw = gfx.textWidth(cond) + 14;
  if (bw < 72) bw = 72;
  gfx.fillRoundRect((W - bw) / 2, BADGE_Y, bw, BADGE_H, 4, col(COL_BADGE_BG));
  gfx.setTextDatum(lgfx::middle_center);
  gfx.setTextColor(col(COL_WHITE), col(COL_BADGE_BG));
  gfx.drawString(cond, W / 2, BADGE_Y + BADGE_H / 2);
  gfx.setTextDatum(lgfx::top_left);

  gfx.fillRect(0, MARQ_Y, W, MARQ_H, col(COL_BLACK)); // stale marquee pixels
  rebuildMarquee();

  // Gauge row: temp left, humidity right.
  gfx.fillRect(0, GAUGE_Y, W, GAUGE_H, col(COL_BLACK));
  // x layout (round 3): temp icon in from the bezel; slimmer bars so the
  // temp label clears the humidity icon and the % label clears the right edge.
  drawIcon(gfx, WX_GAUGE_TEMP, 10, GAUGE_Y + 2);
  drawIcon(gfx, WX_GAUGE_HUMI, 130, GAUGE_Y + 5);
  auto bar = [&](int x, float frac, uint32_t c) {
    gfx.drawRect(x, GAUGE_Y + 9, 50, 14, col(COL_WHITE));
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    gfx.fillRect(x + 2, GAUGE_Y + 11, (int)(46 * frac), 10, col(c));
  };
  bar(32, (r.tempC + 50.0f) / 100.0f, COL_TEMPBAR); // SmolTV-Pro's -50..50 range
  bar(152, r.humidity / 100.0f, COL_BADGE_BG);
  gfx.setFont(&fText.font);
  gfx.setTextColor(col(COL_WHITE), col(COL_BLACK));
  gfx.drawString(r.valid ? fmtTemp(r.tempC, units) : "--", 88, GAUGE_Y + 11);
  gfx.drawString(r.valid ? String(r.humidity) + "%" : "--", 206, GAUGE_Y + 11);
}

void WeatherScreen::rebuildMarquee() {
  const WeatherData::Reading& r = cachedReading;
  if (!r.valid) { // no fetch yet: a scroll of zeros would read as data
    marqWidth = 0; // tick() skips the push; drawWeather cleared the band
    return;
  }
  struct Seg {
    String text;
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
  // The fixed-size sprite (allocated once in begin()) holds ONE copy of the
  // line; tick() tiles it across the band. A line wider than the sprite
  // clips — at 15 px the full line fits MARQ_W_MAX with margin.
  marq.setFont(&fText.font);
  // The tile stride must equal the sprite width exactly: a shorter stride
  // makes each push's black tail erase the start of the NEXT copy, which the
  // following frame repaints — a flicker on the line's first words (seen
  // on-device, round 3). Text shorter than the sprite just reads as a gap
  // between repeats; text longer clips (never happens at 15 px).
  marqWidth = MARQ_W_MAX;
  marqX = 0;
  marq.fillSprite(col(COL_BLACK));
  marq.setTextDatum(lgfx::top_left);
  int x = 0;
  for (const Seg& s : segs) {
    marq.setTextColor(col(s.color), col(COL_BLACK));
    marq.drawString(s.text, x, 2);
    x += marq.textWidth(s.text);
  }
}

void WeatherScreen::drawClock(lgfx::LovyanGFX& gfx, const struct tm& tm) {
  int hour = tm.tm_hour;
  if (!h24) {
    hour = hour % 12;
    if (hour == 0) hour = 12; // 12 h mode: no AM/PM, no leading zero (parity)
  }
  char hs[6], ms[3];
  snprintf(hs, sizeof(hs), h24 ? "%02d:" : "%d:", hour);
  snprintf(ms, sizeof(ms), "%02d", tm.tm_min);

  gfx.fillRect(0, CLOCK_Y, W, CLOCK_H, col(COL_BLACK));
  gfx.setFont(&fClock.font);
  int wh = gfx.textWidth(hs), wm = gfx.textWidth(ms);
  int x0 = (W - wh - wm) / 2;
  gfx.setTextDatum(lgfx::top_left);
  gfx.setTextColor(col(colHour), col(COL_BLACK)); // colon inherits hour color (#67)
  gfx.drawString(hs, x0, CLOCK_Y);
  gfx.setTextColor(col(colMin), col(COL_BLACK));
  gfx.drawString(ms, x0 + wh, CLOCK_Y);
  // The seconds label sits inside this band; -1 differs from every real
  // second, so the change check in tick() repaints it next pass.
  lastSec = -1;
}

void WeatherScreen::drawSeconds(lgfx::LovyanGFX& gfx, int sec) {
  char s[3];
  snprintf(s, sizeof(s), "%02d", sec);
  gfx.setFont(&fSec.font);
  gfx.setTextDatum(lgfx::top_left);
  gfx.setTextColor(col(colSec), col(COL_BLACK));
  gfx.fillRect(SEC_X, SEC_Y, W - SEC_X, 30, col(COL_BLACK));
  gfx.drawString(s, SEC_X, SEC_Y);
}

void WeatherScreen::drawDate(lgfx::LovyanGFX& gfx, const struct tm& tm) {
  char buf[24];
  strftime(buf, sizeof(buf), dateFmt.c_str(), &tm);
  const char* wd = WDAY[tm.tm_wday];

  gfx.fillRect(0, DATE_Y, W, DATE_H, col(COL_BLACK));
  gfx.setFont(&fDate.font);
  int dw = gfx.textWidth(buf), ww = gfx.textWidth(wd);
  int x0 = (W - dw - ww - 6) / 2;
  gfx.setTextDatum(lgfx::top_left);
  gfx.setTextColor(col(COL_WHITE), col(COL_BLACK));
  gfx.drawString(buf, x0, DATE_Y);
  gfx.setTextColor(col(COL_WDAY), col(COL_BLACK));
  gfx.drawString(wd, x0 + dw + 6, DATE_Y);
}
