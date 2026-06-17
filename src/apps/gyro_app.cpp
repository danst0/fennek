// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "gyro_app.h"
#include "config.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "services/gyro.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK

namespace {

using gui::Rect;

constexpr int W   = EINK_W;
constexpr int TOP = appmgr::CONTENT_Y;

const Rect kBack{6, 270, 104, 42};

// Lade-Ablauf: erst „Laden"-Screen zeichnen (eigener tick), dann den ~10 s
// blockierenden Firmware-Upload — sonst friert das Bild ohne Hinweis ein.
enum Phase { NEED_LOAD, LOADING, RUNNING, FAILED };
Phase    s_phase = NEED_LOAD;
uint32_t s_lastDraw = 0;

void drawCentered(Adafruit_GFX& g, int y, const char* s, uint8_t size) {
  uint16_t w, h;
  g.setTextSize(size);
  gui::textBounds(g, s, &w, &h);
  gui::printAt(g, (W - (int)w) / 2, y, s, size);
}

class GyroApp : public App {
 public:
  const char* id()   const override { return "Gyro"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppGyro); }

  void onEnter() override { s_phase = NEED_LOAD; appmgr::markDirty(); }
  void onLeave() override { gyro::end(); }

  void tick() override {
    if (s_phase == NEED_LOAD) { s_phase = LOADING; appmgr::markDirty(); return; }
    if (s_phase == LOADING) {            // blockt beim ersten Mal ~10 s
      s_phase = gyro::begin() ? RUNNING : FAILED;
      appmgr::markDirty();
      return;
    }
    if (s_phase == RUNNING) {
      gyro::poll();
      if (millis() - s_lastDraw >= 400) { s_lastDraw = millis(); appmgr::markDirty(); }
    }
  }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::TAP && kBack.hit(e.x, e.y)) { appmgr::goHome(); return; }
    if (e.type == InputEvent::KEY && (e.key == 'q' || e.key == 'Q' || e.key == '\b'))
      appmgr::goHome();
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    gui::printAt(g, 10, TOP + 6, "IMU / Lage", 3);

    if (s_phase == NEED_LOAD || s_phase == LOADING) {
      drawCentered(g, TOP + 90, "BHI260 Firmware laedt", 2);
      drawCentered(g, TOP + 118, "(~10 s, einmalig)", 1);
    } else if (s_phase == FAILED) {
      drawCentered(g, TOP + 100, "Sensor nicht verfuegbar", 2);
    } else {
      const gyro::Data& d = gyro::current();
      char l[40];
      g.setTextSize(1);
      g.setCursor(14, TOP + 44);  gui::print(g, "Beschleunigung (g)");
      g.setTextSize(2);
      snprintf(l, sizeof(l), "X %+6.2f", d.ax); g.setCursor(20, TOP + 58);  gui::print(g, l);
      snprintf(l, sizeof(l), "Y %+6.2f", d.ay); g.setCursor(20, TOP + 80);  gui::print(g, l);
      snprintf(l, sizeof(l), "Z %+6.2f", d.az); g.setCursor(20, TOP + 102); gui::print(g, l);

      g.setTextSize(1);
      g.setCursor(14, TOP + 130); gui::print(g, "Drehrate (Grad/s)");
      g.setTextSize(2);
      snprintf(l, sizeof(l), "X %+6.1f", d.gx); g.setCursor(20, TOP + 144); gui::print(g, l);
      snprintf(l, sizeof(l), "Y %+6.1f", d.gy); g.setCursor(20, TOP + 166); gui::print(g, l);
      snprintf(l, sizeof(l), "Z %+6.1f", d.gz); g.setCursor(20, TOP + 188); gui::print(g, l);
    }

    gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnBack), false);
  }
};

GyroApp s_app;

}  // namespace

namespace gyro_app {

App* get() { return &s_app; }

}  // namespace gyro_app
