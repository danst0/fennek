#pragma once

// =============================================================================
// Statusicons — shared title-bar status icons (MECK_SIMPLE_LAUNCHER builds)
//
// Drawn top-right on the launcher and the simple settings list (8x8):
//   WiFi arcs   — normal while connected, struck through otherwise
//   Antenna     — normal while the LoRa mesh is enabled, struck through off
// =============================================================================

#ifdef MECK_SIMPLE_LAUNCHER

#include <Arduino.h>
#include <WiFi.h>
#include <helpers/ui/DisplayDriver.h>
#include "../NodePrefs.h"

// WiFi: arcs + dot (8x8, MSB-first, 1 byte/row)
static const uint8_t st_icon_wifi[] PROGMEM = {
  0x3C, 0x42, 0x18, 0x24, 0x00, 0x18, 0x18, 0x00,
};

// Antenna with radio waves (8x8)
static const uint8_t st_icon_antenna[] PROGMEM = {
  0x18, 0x5A, 0x99, 0x18, 0x18, 0x18, 0x3C, 0x00,
};

// GPS: location pin (8x8)
static const uint8_t st_icon_gps[] PROGMEM = {
  0x3C, 0x42, 0x5A, 0x5A, 0x42, 0x24, 0x18, 0x00,
};

// Diagonal strikethrough over an 8x8 icon at (x, y). Clears a diagonal
// channel first so the strike line stays visible on top of dark icon
// pixels (a plain dark-on-dark line was invisible).
inline void meckStrikeIcon(DisplayDriver& display, int x, int y) {
  display.setColor(DisplayDriver::DARK);
  for (int i = -1; i <= 8; i++) {
    display.fillRect(x + i - 1, y + 7 - i, 3, 1);  // cleared channel
  }
  display.setColor(DisplayDriver::LIGHT);
  for (int i = 0; i < 8; i++) {
    display.fillRect(x + i, y + 7 - i, 1, 1);      // strike line
  }
}

// Draw the status icons right-aligned in the title row: active = normal,
// inactive = struck through. Order (right to left): antenna, WiFi, GPS.
inline void meckDrawStatusIcons(DisplayDriver& display, const NodePrefs* prefs) {
  display.setColor(DisplayDriver::LIGHT);

  // Antenna (rightmost)
  int x = 128 - 9;
  display.drawXbm(x, 1, st_icon_antenna, 8, 8);
  if (!prefs || !prefs->lora_enabled) meckStrikeIcon(display, x, 1);

  // WiFi
  x -= 12;
  display.setColor(DisplayDriver::LIGHT);
  display.drawXbm(x, 1, st_icon_wifi, 8, 8);
  if (WiFi.status() != WL_CONNECTED) meckStrikeIcon(display, x, 1);

  // GPS
  x -= 12;
  display.setColor(DisplayDriver::LIGHT);
  display.drawXbm(x, 1, st_icon_gps, 8, 8);
  if (!prefs || !prefs->gps_enabled) meckStrikeIcon(display, x, 1);

  display.setColor(DisplayDriver::LIGHT);
}

#endif // MECK_SIMPLE_LAUNCHER
