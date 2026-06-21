// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "files_app.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "core/settings.h"
#include "services/webfm.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <stdio.h>
#include <string.h>

namespace {

using gui::Rect;

constexpr Rect kBtnStart = {70, 150, 100, 40};
constexpr Rect kBack     = {6, 286, 72, 28};   // „Zurück" (eine Ebene): Server aus bzw. Launcher

// Letzter gezeichneter Zustand (tick() erkennt Änderungen -> markDirty).
webfm::State s_shownState = webfm::State::OFF;
uint32_t     s_shownReqs = 0;
uint32_t     s_lastReqDraw = 0;

bool hasCreds() {
  char ssid[33];
  settings::wifiSsid(ssid, sizeof(ssid));
  return ssid[0] != '\0';
}

void centered(Adafruit_GFX& g, int y, const char* text, uint8_t size) {
  g.setTextSize(size);
  uint16_t w, h;
  gui::textBounds(g, text, &w, &h);
  gui::printAt(g, (240 - (int)w) / 2, y, text, size);
}

void drawNoCreds(Adafruit_GFX& g) {
  centered(g, 90, i18n::tr(i18n::Str::FilesNoCreds1), 1);
  centered(g, 112, i18n::tr(i18n::Str::FilesNoCreds2), 1);
  gui::printAt(g, 40, 128, "WLAN-SSID / WLAN-Passwort", 1);
  centered(g, 152, i18n::tr(i18n::Str::FilesNoCreds3), 1);
  // Konsolen-Befehle wörtlich (nicht übersetzt, vgl. core/console.cpp).
  gui::printAt(g, 40, 168, "wifi ssid <Name>", 1);
  gui::printAt(g, 40, 184, "wifi pass <Passwort>", 1);
}

void drawIdle(Adafruit_GFX& g) {
  char ssid[33], line[48];
  webfm::ssid(ssid, sizeof(ssid));
  snprintf(line, sizeof(line), i18n::tr(i18n::Str::FmtFilesSsid), ssid);
  centered(g, 100, line, 1);
  gui::drawButton(g, kBtnStart, i18n::tr(i18n::Str::BtnStart), true);
  centered(g, 210, i18n::tr(i18n::Str::FilesStartHint), 1);
}

void drawConnecting(Adafruit_GFX& g) {
  char ssid[33], line[48];
  webfm::ssid(ssid, sizeof(ssid));
  snprintf(line, sizeof(line), i18n::tr(i18n::Str::FmtFilesSsid), ssid);
  centered(g, 100, line, 1);
  centered(g, 140, i18n::tr(i18n::Str::FilesConnecting), 2);
}

void drawRunning(Adafruit_GFX& g) {
  char ip[16], line[48];
  webfm::ipStr(ip, sizeof(ip));
  centered(g, 90, i18n::tr(i18n::Str::FilesBrowserHint), 1);
  snprintf(line, sizeof(line), "http://%s/", ip);
  centered(g, 110, line, 2);
  centered(g, 140, "http://fennek.local/", 1);
  snprintf(line, sizeof(line), i18n::tr(i18n::Str::FmtRequests),
           (unsigned long)webfm::requestCount());
  centered(g, 190, line, 1);
  centered(g, 230, i18n::tr(i18n::Str::FilesStopHint), 1);
}

void drawFailed(Adafruit_GFX& g) {
  centered(g, 110, i18n::tr(i18n::Str::FilesFailed), 1);
  centered(g, 140, i18n::tr(i18n::Str::FilesRetryHint), 1);
}

class FilesApp : public App {
 public:
  const char* id()   const override { return "Dateien"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppFiles); }

  void onEnter() override {
    s_shownState = webfm::state();
    s_shownReqs = webfm::requestCount();
  }

  // Verlassen der App = Server aus (vorhersehbar, kein Dauer-Stromfresser).
  void onLeave() override { webfm::stop(); }

  void handleInput(const InputEvent& e) override {
    // Sichtbarer „Zurück"-Knopf = wie Q/Backspace: läuft der Server, erst stoppen;
    // sonst zum Launcher.
    if (e.type == InputEvent::TAP && kBack.hit(e.x, e.y)) {
      if (webfm::state() != webfm::State::OFF) { webfm::stop(); appmgr::markDirty(); }
      else appmgr::goHome();
      return;
    }
    bool activate = false;
    if (e.type == InputEvent::TAP) {
      activate = (webfm::state() == webfm::State::OFF) ? kBtnStart.hit(e.x, e.y)
                                                       : true;
    } else if (e.key == '\r') {
      activate = true;
    } else if (e.key == 'q' || e.key == 'Q' || e.key == '\b') {
      if (webfm::state() != webfm::State::OFF) {
        webfm::stop();
        appmgr::markDirty();
      } else {
        appmgr::goHome();
      }
      return;
    }
    if (!activate) return;

    webfm::State st = webfm::state();
    if ((st == webfm::State::OFF || st == webfm::State::FAILED) && hasCreds()) {
      webfm::start();
      appmgr::markDirty();
    }
  }

  void tick() override {
    if (appmgr::isDirty()) return;
    webfm::State st = webfm::state();
    if (st != s_shownState) {
      s_shownState = st;
      appmgr::markDirty();
      return;
    }
    // Request-Zähler gedrosselt nachziehen (max. alle 50 s ein Refresh —
    // Invariante 7: E-Ink nur bei echter Änderung, koalesziert). Der Zähler ist
    // reine Info; ein häufigerer Voll-Redraw kostet nur sichtbares E-Ink-Flackern.
    if (st == webfm::State::RUNNING && webfm::requestCount() != s_shownReqs &&
        millis() - s_lastReqDraw > 50000) {
      appmgr::markDirty();
    }
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    centered(g, 44, i18n::tr(i18n::Str::FilesTitle), 2);
    webfm::State st = webfm::state();
    s_shownState = st;
    s_shownReqs = webfm::requestCount();
    s_lastReqDraw = millis();
    switch (st) {
      case webfm::State::OFF:
        if (hasCreds()) drawIdle(g);
        else            drawNoCreds(g);
        break;
      case webfm::State::CONNECTING: drawConnecting(g); break;
      case webfm::State::RUNNING:    drawRunning(g); break;
      case webfm::State::FAILED:     drawFailed(g); break;
    }
    gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnBackShort), false);
  }
};

FilesApp s_app;

}  // namespace

namespace files_app {

App* get() { return &s_app; }

}  // namespace files_app
