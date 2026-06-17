// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "gyro_app.h"
#include "config.h"
#include "core/gui.h"
#include "core/i18n.h"

#include <Arduino.h>
#include <Wire.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK

namespace {

using gui::Rect;

constexpr int W   = EINK_W;
constexpr int TOP = appmgr::CONTENT_Y;
constexpr uint8_t kBhiAddr = 0x28;   // BHI260AP

const Rect kBack{6, 270, 104, 42};

int  s_present = -1;   // -1 = noch nicht geprüft, 0 = nein, 1 = ja

// I2C-Präsenz prüfen (Wire läuft seit main).
void probe() {
  Wire.beginTransmission(kBhiAddr);
  s_present = (Wire.endTransmission() == 0) ? 1 : 0;
}

class GyroApp : public App {
 public:
  const char* id()   const override { return "Gyro"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppGyro); }

  void onEnter() override { probe(); }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::TAP && kBack.hit(e.x, e.y)) { appmgr::goHome(); return; }
    if (e.type == InputEvent::KEY && (e.key == 'q' || e.key == 'Q' || e.key == '\b'))
      appmgr::goHome();
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    gui::printAt(g, 10, TOP + 8, "IMU / Lage", 3);

    g.setTextSize(2);
    g.setCursor(10, TOP + 50);
    gui::print(g, "Sensor: BHI260AP");
    g.setCursor(10, TOP + 76);
    char line[40];
    snprintf(line, sizeof(line), "I2C 0x28: %s",
             s_present == 1 ? "erkannt" : s_present == 0 ? "nicht gefunden" : "...");
    gui::print(g, line);

    // Stufe-2-Hinweis (Live-Daten brauchen den BHy2-Firmware-Upload).
    g.setTextSize(1);
    g.setCursor(10, TOP + 120);
    gui::print(g, "Live-Beschleunigung/Drehrate folgt:");
    g.setCursor(10, TOP + 134);
    gui::print(g, "BHI260 ist ein Smart-Sensor-Hub und");
    g.setCursor(10, TOP + 146);
    gui::print(g, "braucht einen Firmware-Upload (BHy2).");

    gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnBack), false);
  }
};

GyroApp s_app;

}  // namespace

namespace gyro_app {

App* get() { return &s_app; }

}  // namespace gyro_app
