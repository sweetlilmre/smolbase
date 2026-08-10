// Variant C layout (ticket #67 resolution — coordinates live there and in the
// prototype on the prototype/dashboard-layout branch). Drawing goes DIRECT to
// the panel — no full framebuffer: every element owns a black rectangle it
// clears and repaints on its own cadence (dirty-flag pattern, ticket #6), and
// AA text blends toward the drawn background per the asset research. The
// marquee is the exception: a 16-bpp sprite holding the full line, pushed at
// a scrolling offset each frame (LovyanGFX clips to the panel, so only the
// visible band is transmitted).
//
// Fonts are lv_font_conv bin blobs (scripts/build_assets.py) parsed once into
// static BFFfont instances — LovyanGFX's loadFont() slot holds only one
// runtime font, so we hold our own and switch with setFont(&face).
#include "WeatherScreen.h"
#include "../core/ConfigStore.h"
#include "WeatherData.h"
#include "assets/wx_assets.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace {

constexpr int W = 240;
// Element bands (each element clears its own). Fine-tuned on-device in #74.
constexpr int ICON_X = 8, ICON_Y = 8;
constexpr int CITY_Y = 14;
constexpr int BADGE_Y = 44, BADGE_H = 22;
constexpr int MARQ_Y = 74, MARQ_H = 16;
constexpr int CLOCK_Y = 92, CLOCK_H = 92; // the Teko 96 box
constexpr int SEC_X = 196, SEC_Y = 114;
constexpr int DATE_Y = 186, DATE_H = 18;
constexpr int GAUGE_Y = 212, GAUGE_H = 28;

// 24-bit palette (converted per-draw with color888 — raw ints would be read
// as RGB565 on the 16-bpp panel and come out wrong).
constexpr uint32_t COL_AMBER = 0xfeba00, COL_CYAN = 0x99ffff, COL_GREEN = 0x99ff1f;
constexpr uint32_t COL_WDAY = 0x87cefa, COL_BADGE_BG = 0x2196f3, COL_TEMPBAR = 0xe53935;
constexpr uint32_t COL_WHITE = 0xffffff, COL_BLACK = 0x000000;
const char* const WDAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

uint32_t c565(uint32_t rgb) { return lgfx::color565(rgb >> 16, (rgb >> 8) & 0xff, rgb & 0xff); }

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

void drawIcon(lgfx::LGFX_Device& gfx, const WxIcon& ic, int x, int y) {
  gfx.pushImage(x, y, ic.w, ic.h, (const void*)ic.data, 0u, lgfx::palette_4bit,
                (const lgfx::rgb565_t*)ic.palette);
}

const WxIcon& conditionIcon(uint8_t code) {
  for (size_t i = 0; i < WX_ICON_COUNT; ++i)
    if (WX_ICON_CODES[i] == code) return WX_ICONS[i];
  return WX_ICONS[0]; // unknown → clear (SmolTV-Pro's fallback)
}

} // namespace

void WeatherScreen::begin() {
  // A face that fails to parse stays unloaded and its text draws in the
  // fallback font — visible, not fatal. The BFF smoke test belongs to #74.
  fClock.load(WX_CLOCK96);
  fSec.load(WX_SEC40);
  fCity.load(WX_CITY22);
  fDate.load(WX_DATE15);
  fBadge.load(WX_BADGE13);
  fText.load(WX_TEXT12);
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
  weatherDirty = true; // colours/units/format re-render from cached data (#68)
  lastMin = lastSec = lastDay = -1;
}

void WeatherScreen::onEnter(lgfx::LGFX_Device& gfx) {
  gfx.setBaseColor(c565(COL_BLACK));
  gfx.fillScreen(c565(COL_BLACK));
  weatherDirty = true;
  lastMin = lastSec = lastDay = -1;
}

void WeatherScreen::onTap() { WeatherData::forceRefresh(); }

void WeatherScreen::tick(lgfx::LGFX_Device& gfx) {
  if (weatherDirty) {
    weatherDirty = false;
    drawWeather(gfx);
  }
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  if (tm.tm_min != lastMin) {
    lastMin = tm.tm_min;
    drawClock(gfx);
  }
  if (tm.tm_sec != lastSec) {
    lastSec = tm.tm_sec;
    drawSeconds(gfx);
  }
  if (tm.tm_mday != lastDay) {
    lastDay = tm.tm_mday;
    drawDate(gfx);
  }
  // Marquee: ~30 Hz, 1 px/frame ≈ 30 px/s — the original's 16 s feel (#74 tunes).
  uint32_t now = millis();
  if (marqWidth > 0 && now - lastFrameMs >= 33) {
    lastFrameMs = now;
    if (--marqX <= -marqWidth) marqX += marqWidth;
    // Consecutive copies cover the whole band; the panel clips the rest.
    for (int x = marqX; x < W; x += marqWidth) marq.pushSprite(&gfx, x, MARQ_Y);
  }
}

// ---- element painters --------------------------------------------------------

void WeatherScreen::drawWeather(lgfx::LGFX_Device& gfx) {
  const WeatherData::Reading& r = WeatherData::reading();

  // Icon + city + badge share the top band; clear it wholesale.
  gfx.fillRect(0, 0, W, MARQ_Y, c565(COL_BLACK));

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
  gfx.setTextColor(c565(COL_WHITE), c565(COL_BLACK));
  gfx.drawString(city, x0, CITY_Y);
  if (ccw) {
    gfx.setFont(&fBadge.font);
    gfx.setTextColor(c565(COL_AMBER), c565(COL_BLACK));
    gfx.drawString(r.country, x0 + cw + 6, CITY_Y + 7);
  }

  // Condition badge, centered; width grows for long OWM mains ("Thunderstorm").
  const char* cond = r.valid ? r.condition : "--";
  gfx.setFont(&fBadge.font);
  int bw = gfx.textWidth(cond) + 14;
  if (bw < 72) bw = 72;
  gfx.fillRoundRect((W - bw) / 2, BADGE_Y, bw, BADGE_H, 4, c565(COL_BADGE_BG));
  gfx.setTextDatum(lgfx::middle_center);
  gfx.setTextColor(c565(COL_WHITE), c565(COL_BADGE_BG));
  gfx.drawString(cond, W / 2, BADGE_Y + BADGE_H / 2);
  gfx.setTextDatum(lgfx::top_left);

  rebuildMarquee();

  // Gauge row: temp left, humidity right.
  gfx.fillRect(0, GAUGE_Y, W, GAUGE_H, c565(COL_BLACK));
  drawIcon(gfx, WX_GAUGE_TEMP, 10, GAUGE_Y + 3);
  drawIcon(gfx, WX_GAUGE_HUMI, 128, GAUGE_Y + 6);
  auto bar = [&](int x, float frac, uint32_t col) {
    gfx.drawRect(x, GAUGE_Y + 6, 52, 12, c565(COL_WHITE));
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    gfx.fillRect(x + 2, GAUGE_Y + 8, (int)(48 * frac), 8, c565(col));
  };
  bar(28, (r.tempC + 50.0f) / 100.0f, COL_TEMPBAR); // SmolTV-Pro's -50..50 range
  bar(146, r.humidity / 100.0f, COL_BADGE_BG);
  gfx.setFont(&fText.font);
  gfx.setTextColor(c565(COL_WHITE), c565(COL_BLACK));
  gfx.drawString(r.valid ? WeatherData::fmtTemp(r.tempC) : "--", 85, GAUGE_Y + 7);
  gfx.drawString(r.valid ? String(r.humidity) + "%" : "--", 203, GAUGE_Y + 7);
}

void WeatherScreen::rebuildMarquee() {
  const WeatherData::Reading& r = WeatherData::reading();
  struct Seg {
    String text;
    uint32_t color;
  };
  const Seg segs[] = {
      {"Lowest ", COL_WHITE},       {WeatherData::fmtTemp(r.tempMinC), COL_CYAN},
      {", Highest ", COL_WHITE},    {WeatherData::fmtTemp(r.tempMaxC), COL_AMBER},
      {", Feels like ", COL_WHITE}, {WeatherData::fmtTemp(r.feelsC), COL_AMBER},
      {", Wind speed ", COL_WHITE}, {WeatherData::fmtWind(r.windMs), COL_GREEN},
      {", ATM ", COL_WHITE},        {WeatherData::fmtPress(r.pressureHpa), COL_AMBER},
      {"      ", COL_WHITE},
  };
  // The sprite holds ONE full copy of the line; tick() tiles it across the
  // band. Rebuilt only on a weather/settings change, so the realloc is rare.
  marq.setColorDepth(16);
  marq.setFont(&fText.font);
  int w = 0;
  for (const Seg& s : segs) w += marq.textWidth(s.text);
  if (w < W) w = W; // tiles must at least span the screen
  if (w != marqWidth) {
    marq.deleteSprite();
    if (!marq.createSprite(w, MARQ_H)) { // ~(w*32) B heap; ~16 KB for a full line
      marqWidth = 0;                     // OOM: skip the marquee, keep the rest
      return;
    }
    marqWidth = w;
    marqX = 0;
  }
  marq.fillSprite(marq.color565(0, 0, 0));
  marq.setTextDatum(lgfx::top_left);
  int x = 0;
  for (const Seg& s : segs) {
    marq.setTextColor(marq.color565(s.color >> 16, (s.color >> 8) & 0xff, s.color & 0xff),
                      marq.color565(0, 0, 0));
    marq.drawString(s.text, x, 2);
    x += marq.textWidth(s.text);
  }
}

void WeatherScreen::drawClock(lgfx::LGFX_Device& gfx) {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  int hour = tm.tm_hour;
  if (!h24) {
    hour = hour % 12;
    if (hour == 0) hour = 12; // 12 h mode: no AM/PM, no leading zero (parity)
  }
  char hs[6], ms[3];
  snprintf(hs, sizeof(hs), h24 ? "%02d:" : "%d:", hour);
  snprintf(ms, sizeof(ms), "%02d", tm.tm_min);

  gfx.fillRect(0, CLOCK_Y, W, CLOCK_H, c565(COL_BLACK));
  gfx.setFont(&fClock.font);
  int wh = gfx.textWidth(hs), wm = gfx.textWidth(ms);
  int x0 = (W - wh - wm) / 2;
  gfx.setTextDatum(lgfx::top_left);
  gfx.setTextColor(c565(colHour), c565(COL_BLACK)); // colon inherits hour color (#67)
  gfx.drawString(hs, x0, CLOCK_Y);
  gfx.setTextColor(c565(colMin), c565(COL_BLACK));
  gfx.drawString(ms, x0 + wh, CLOCK_Y);
  lastSec = -1; // the seconds label sits inside this band; repaint next tick
}

void WeatherScreen::drawSeconds(lgfx::LGFX_Device& gfx) {
  if (lastSec < 0) return;
  char s[3];
  snprintf(s, sizeof(s), "%02d", lastSec);
  gfx.setFont(&fSec.font);
  gfx.setTextDatum(lgfx::top_left);
  gfx.setTextColor(c565(colSec), c565(COL_BLACK));
  gfx.fillRect(SEC_X, SEC_Y, W - SEC_X, 44, c565(COL_BLACK));
  gfx.drawString(s, SEC_X, SEC_Y);
}

void WeatherScreen::drawDate(lgfx::LGFX_Device& gfx) {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[24];
  strftime(buf, sizeof(buf), dateFmt.c_str(), &tm);
  const char* wd = WDAY[tm.tm_wday];

  gfx.fillRect(0, DATE_Y, W, DATE_H, c565(COL_BLACK));
  gfx.setFont(&fDate.font);
  int dw = gfx.textWidth(buf), ww = gfx.textWidth(wd);
  int x0 = (W - dw - ww - 6) / 2;
  gfx.setTextDatum(lgfx::top_left);
  gfx.setTextColor(c565(COL_WHITE), c565(COL_BLACK));
  gfx.drawString(buf, x0, DATE_Y);
  gfx.setTextColor(c565(COL_WDAY), c565(COL_BLACK));
  gfx.drawString(wd, x0 + dw + 6, DATE_Y);
}
