#include "SystemScreens.h"
#include "Net.h"

void ApInfoScreen::onEnter(lgfx::LGFX_Device& d) {
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(lgfx::middle_center);
  d.setTextSize(1);
  d.setFont(&fonts::FreeSans9pt7b);
  d.drawString("Wi-Fi setup", 120, 60);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.drawString(Net::deviceName(), 120, 110);
  d.setFont(&fonts::FreeSans9pt7b);
  d.drawString("join this network, then open", 120, 150);
  d.drawString("http://" + Net::ip().toString(), 120, 180);
}

ApInfoScreen& apInfoScreen() {
  static ApInfoScreen s;
  return s;
}
