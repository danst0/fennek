// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "touch.h"
#include "config.h"
#include "hyn/HynTouch.h"

#include <Arduino.h>

namespace {
bool s_ready = false;

// --- Achsen-Korrektur -------------------------------------------------------
// Display (GxEPD2, Rotation 0) und Touch-Panel sollten 1:1 passen. Falls auf
// der Hardware Tap-Position und Anzeige nicht übereinstimmen, hier umstellen.
constexpr bool kSwapXY   = false;
constexpr bool kReverseX = false;
constexpr bool kReverseY = false;
}  // namespace

namespace touch {

bool begin() {
  HynTouchConfig cfg = hyn_touch_default_config();
  cfg.sda_pin      = PIN_I2C_SDA;
  cfg.scl_pin      = PIN_I2C_SCL;
  cfg.reset_pin    = PIN_TOUCH_RST;
  cfg.irq_pin      = PIN_TOUCH_INT;
  cfg.x_resolution = EINK_W;
  cfg.y_resolution = EINK_H;
  s_ready = hyn_touch_init_with_config(&cfg);
  return s_ready;
}

bool poll(int16_t& x, int16_t& y) {
  if (!s_ready) return false;
  int16_t hx[1], hy[1];
  if (hyn_touch_get_point(hx, hy, 1) <= 0) return false;

  int16_t rx = hx[0], ry = hy[0];
  if (kSwapXY) { int16_t t = rx; rx = ry; ry = t; }
  if (kReverseX) rx = (EINK_W - 1) - rx;
  if (kReverseY) ry = (EINK_H - 1) - ry;

  // Clamp auf Display-Grenzen.
  if (rx < 0) rx = 0; else if (rx >= EINK_W) rx = EINK_W - 1;
  if (ry < 0) ry = 0; else if (ry >= EINK_H) ry = EINK_H - 1;
  x = rx; y = ry;
  return true;
}

void flush() {
  if (!s_ready) return;
  int16_t hx[1], hy[1];
  hyn_touch_get_point(hx, hy, 1);   // löscht das press_flag, Punkt wird verworfen
}

}  // namespace touch
