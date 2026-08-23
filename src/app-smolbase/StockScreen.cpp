#include "StockScreen.h"
#include "../core/Clock.h"
#include "../core/ConfigStore.h"
#include "../core/Net.h"
#include "hex_color.h"
#include <cstdio>
#include <ctime>

void StockScreen::loadColors() {
  colHour = hexRgb(ConfigStore::getString("col_hour"), 0xffffff);
  colMin = hexRgb(ConfigStore::getString("col_min"), 0xffffff);
  colColon = hexRgb(ConfigStore::getString("col_colon"), 0xffffff);
  colHost = hexRgb(ConfigStore::getString("col_host"), 0xffffff);
  colIp = hexRgb(ConfigStore::getString("col_ip"), 0xffffff);
}

void StockScreen::drawColon(lgfx::LGFX_Device& d) {
  d.setFont(&fonts::FreeSansBold24pt7b);
  int w = d.textWidth(":");
  d.fillRect(120 - w / 2, 70, w, 60, TFT_BLACK);
  if (colonOn) {
    d.setTextColor(d.color888(colColon >> 16, (colColon >> 8) & 0xff, colColon & 0xff), TFT_BLACK);
    d.setTextDatum(lgfx::middle_center);
    d.drawString(":", 120, 100);
  }
}

void StockScreen::onEnter(lgfx::LGFX_Device& d) {
  d.fillScreen(TFT_BLACK);
  dirty = true;
  lastMinute = -1;
}

void StockScreen::tick(lgfx::LGFX_Device& d) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  bool colonNow = !Clock::isSynced() || (t.tm_sec & 1) == 0;
  if (!dirty && t.tm_min == lastMinute) {
    if (colonNow != colonOn) {
      colonOn = colonNow;
      drawColon(d);
    }
    return;
  }
  lastMinute = t.tm_min;
  dirty = false;
  colonOn = colonNow;
  loadColors();

  d.setFont(&fonts::FreeSansBold24pt7b);
  int w = d.textWidth(":");
  char hh[3] = "--", mm[3] = "--";
  if (Clock::isSynced()) {
    snprintf(hh, sizeof(hh), "%02d", t.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", t.tm_min);
  }
  d.fillRect(0, 70, 240, 60, TFT_BLACK);
  d.setTextColor(d.color888(colHour >> 16, (colHour >> 8) & 0xff, colHour & 0xff), TFT_BLACK);
  d.setTextDatum(lgfx::middle_right);
  d.drawString(hh, 120 - w / 2, 100);
  d.setTextColor(d.color888(colMin >> 16, (colMin >> 8) & 0xff, colMin & 0xff), TFT_BLACK);
  d.setTextDatum(lgfx::middle_left);
  d.drawString(mm, 120 + w / 2, 100);
  drawColon(d);

  d.setFont(&fonts::FreeSans9pt7b);
  d.setTextDatum(lgfx::middle_center);
  d.fillRect(0, 145, 240, 65, TFT_BLACK);
  d.setTextColor(d.color888(colHost >> 16, (colHost >> 8) & 0xff, colHost & 0xff), TFT_BLACK);
  // Stack buffer — see the note in DemoScreen::drawIdentity.
  char host[48];
  snprintf(host, sizeof(host), "%s.local", Net::deviceName().c_str());
  d.drawString(host, 120, 160);
  d.setTextColor(d.color888(colIp >> 16, (colIp >> 8) & 0xff, colIp & 0xff), TFT_BLACK);
  d.drawString(Net::isUp() ? Net::ip().c_str() : "connecting...", 120, 185);
}
