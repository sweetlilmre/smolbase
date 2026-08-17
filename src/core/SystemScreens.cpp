#include "SystemScreens.h"
#include "Net.h"
#include "smolbase_config.h"

namespace {
// One palette for both screens, so the join screen and the AP screen read as
// the same family: deep blue header band, light blue boxes, grey labels.
constexpr uint8_t BAND[3] = {16, 48, 96};
constexpr uint8_t ACCENT[3] = {80, 170, 255};
constexpr uint8_t DIM[3] = {170, 170, 170};

// Progress bar geometry, shared between the once-painted outline and the
// per-tick fill so the two cannot drift apart.
constexpr int BAR_X = 14, BAR_Y = 150, BAR_W = 212, BAR_H = 12;
constexpr int FILL_X = BAR_X + 2, FILL_Y = BAR_Y + 2;
constexpr int FILL_W = BAR_W - 4, FILL_H = BAR_H - 4;
constexpr int NOTE_Y = 186; // the countdown line, repainted in place
constexpr uint32_t PAINT_MS = 200;
} // namespace

// Draws direct to the device (never via Display::frame) so it renders identically in
// every SMOLBASE_FRAMEBUFFER mode. Full paint here; nothing changes until exit, so
// tick() stays a no-op.
void ApInfoScreen::onEnter(lgfx::LGFX_Device& d) {
  const uint32_t band = d.color888(BAND[0], BAND[1], BAND[2]);
  const uint32_t accent = d.color888(ACCENT[0], ACCENT[1], ACCENT[2]);
  const uint32_t dim = d.color888(DIM[0], DIM[1], DIM[2]);

  d.fillScreen(TFT_BLACK);
  d.setTextSize(1);
  d.setTextDatum(lgfx::middle_center);

  // Title band
  d.fillRect(0, 0, 240, 44, band);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextColor(TFT_WHITE);
  d.drawString("Wi-Fi Setup", 120, 23);

  // Step 1: join the device's AP
  d.setFont(&fonts::FreeSans9pt7b);
  d.setTextColor(dim);
  d.drawString("1. Join this network", 120, 72);
  d.drawRoundRect(14, 92, 212, 40, 8, accent);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextColor(TFT_WHITE);
  d.drawString(Net::deviceName(), 120, 112);

  // Step 2: open the captive portal
  d.setFont(&fonts::FreeSans9pt7b);
  d.setTextColor(dim);
  d.drawString("2. Open in a browser", 120, 162);
  d.drawRoundRect(14, 182, 212, 40, 8, accent);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(accent);
  d.drawString("http://" + Net::ip().toString(), 120, 202); // 192.168.4.1 in AP mode
}

ApInfoScreen& apInfoScreen() {
  static ApInfoScreen s;
  return s;
}

// The boot join, in the AP screen's idiom: title band, a label, the network in
// a rounded box, and — the one thing the AP screen has no need of — a bar for
// the SMOLBASE_CONNECT_TIMEOUT_MS the stored credentials get before the device
// gives up and raises its own AP. The bar is the honest thing to show here:
// this wait has a deadline and a known consequence, so the screen says what
// happens when it runs out rather than spinning forever.
void WifiJoinScreen::onEnter(lgfx::LGFX_Device& d) {
  const uint32_t band = d.color888(BAND[0], BAND[1], BAND[2]);
  const uint32_t accent = d.color888(ACCENT[0], ACCENT[1], ACCENT[2]);
  const uint32_t dim = d.color888(DIM[0], DIM[1], DIM[2]);
  lastPaintMs = 0; // paint the animated half on the very first tick
  barFilled = 0;
  lastSecs = -1;

  d.fillScreen(TFT_BLACK);
  d.setTextSize(1);
  d.setTextDatum(lgfx::middle_center);

  d.fillRect(0, 0, 240, 44, band);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextColor(TFT_WHITE);
  d.drawString("Connecting", 120, 23);

  d.setFont(&fonts::FreeSans9pt7b);
  d.setTextColor(dim);
  d.drawString("Joining network", 120, 72);

  // The SSID is user data of arbitrary length: drop a size if it would run past
  // the box, and let LovyanGFX clip anything still too long for the panel.
  d.drawRoundRect(14, 92, 212, 40, 8, accent);
  const String ssid = Net::joiningSsid();
  d.setFont(&fonts::FreeSansBold12pt7b);
  if (d.textWidth(ssid) > 196) d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(TFT_WHITE);
  d.drawString(ssid, 120, 112);

  d.drawRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, 3, dim);

  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(accent);
  d.drawString(Net::deviceName(), 120, 214);
}

// Only the bar and the countdown move. The bar grows by a sliver (it never
// shrinks, so there is nothing to erase), and the countdown line is the one
// place that clears itself before redrawing — 20 rows at 5 Hz.
void WifiJoinScreen::tick(lgfx::LGFX_Device& d) {
  const uint32_t now = millis();
  if (lastPaintMs && now - lastPaintMs < PAINT_MS) return;
  lastPaintMs = now;

  const uint32_t elapsed = Net::joinElapsedMs();
  const uint32_t total = SMOLBASE_CONNECT_TIMEOUT_MS;
  const uint32_t clamped = elapsed > total ? total : elapsed;

  const int want = (int)((uint64_t)FILL_W * clamped / total);
  if (want > barFilled) {
    d.fillRect(FILL_X + barFilled, FILL_Y, want - barFilled, FILL_H,
               d.color888(ACCENT[0], ACCENT[1], ACCENT[2]));
    barFilled = want;
  }

  // Round up, so the line reads "1 s" for the whole final second rather than
  // sitting on "0 s" while the timeout has visibly not fired yet.
  const int secs = (int)((total - clamped + 999) / 1000);
  if (secs == lastSecs) return;
  lastSecs = secs;
  d.fillRect(0, NOTE_Y - 10, 240, 20, TFT_BLACK);
  d.setFont(&fonts::FreeSans9pt7b);
  d.setTextDatum(lgfx::middle_center);
  d.setTextColor(d.color888(DIM[0], DIM[1], DIM[2]));
  d.drawString("Wi-Fi Setup in " + String(secs) + "s", 120, NOTE_Y);
}

WifiJoinScreen& wifiJoinScreen() {
  static WifiJoinScreen s;
  return s;
}
