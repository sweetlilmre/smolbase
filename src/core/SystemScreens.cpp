#include "SystemScreens.h"
#include "Net.h"

// Draws direct to the device (never via Display::frame) so it renders identically in
// every SMOLBASE_FRAMEBUFFER mode. Full paint here; nothing changes until exit, so
// tick() stays a no-op.
void ApInfoScreen::onEnter(lgfx::LGFX_Device& d) {
  const uint32_t band = d.color888(16, 48, 96);    // header: deep blue
  const uint32_t accent = d.color888(80, 170, 255); // boxes + URL: light blue
  const uint32_t dim = d.color888(170, 170, 170);   // step labels: grey

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
