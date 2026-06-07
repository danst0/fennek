#pragma once

// =============================================================================
// SimpleSettingsScreen — flat settings list (MECK_SIMPLE_LAUNCHER builds)
//
// Opened by the launcher's Settings entry. The full Meck settings moved one
// level down behind "Advanced >>".
//
//   WiFi          [switch]   — toggles the radio, joins /web/wifi.cfg network
//   LoRa Mesh     [switch]   — SX1262 sleep + mesh loop pause, persisted
//   GPS           [switch]   — UITask::toggleGPS() (pref + hardware power)
//   File Manager     >>      — SD web file manager (STA mode, IP on screen)
//   Advanced         >>      — the complete stock Meck settings
//
// Input: tap a row to toggle/open it directly; keyboard w/s + Enter;
// q/Esc or a tap on the title bar returns to the launcher.
// =============================================================================

#ifdef MECK_SIMPLE_LAUNCHER

#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/ui/DisplayDriver.h>
#include "../NodePrefs.h"
#include "UITask.h"
#include "Settingsscreen.h"

class SimpleSettingsScreen : public UIScreen {
public:
  static const int ROW_COUNT = 5;
  enum Row : uint8_t { ROW_WIFI, ROW_LORA, ROW_GPS, ROW_FILEMGR, ROW_ADVANCED };

  // Layout (virtual 128x128): title at top, rows below, footer for WiFi info
  static const int LIST_TOP = 20;
  static const int ROW_H = 16;

  SimpleSettingsScreen(UITask* task, NodePrefs* prefs)
    : _task(task), _prefs(prefs), _sel(0) {}

  int render(DisplayDriver& display) override {
    static const char* LABELS[ROW_COUNT] = {
      "WiFi", "LoRa Mesh", "GPS", "File Manager", "Advanced",
    };

    display.setColor(DisplayDriver::GREEN);
    display.setTextSize(_prefs->smallTextSize());
    display.setCursor(2, 4);
    display.print("Settings");
    display.setColor(DisplayDriver::LIGHT);

    for (int i = 0; i < ROW_COUNT; i++) {
      int y = LIST_TOP + i * ROW_H;

      // Selection marker: border around the active row
      if (i == _sel) {
        display.drawRect(0, y - 2, 128, ROW_H - 2);
      }

      display.setCursor(4, y + 2);
      display.print(LABELS[i]);

      if (i == ROW_FILEMGR || i == ROW_ADVANCED) {
        display.setCursor(110, y + 2);
        display.print(">>");
      } else {
        drawSwitch(display, 104, y + 3, rowState((Row)i));
      }
    }

    // Footer: WiFi connection info
    display.setTextSize(0);
    if (WiFi.status() == WL_CONNECTED) {
      char buf[48];
      snprintf(buf, sizeof(buf), "%s  %s",
               WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      display.setColor(DisplayDriver::GREEN);
      display.setCursor(2, 118);
      display.print(buf);
      display.setColor(DisplayDriver::LIGHT);
    }
    display.setTextSize(1);

    return 2000;  // WiFi status may change in the background
  }

  bool handleInput(char c) override {
    if (c == 'w' || c == KEY_UP || c == KEY_PREV) {
      _sel = (_sel + ROW_COUNT - 1) % ROW_COUNT;
      return true;
    }
    if (c == 's' || c == KEY_DOWN || c == KEY_NEXT) {
      _sel = (_sel + 1) % ROW_COUNT;
      return true;
    }
    if (c == KEY_ENTER || c == KEY_SELECT) {
      activate((Row)_sel);
      return true;
    }
    if (c == 'q' || c == KEY_CANCEL) {
      _task->gotoHomeScreen();  // back to the launcher
      return true;
    }
    return false;
  }

  // Touch: select AND activate the row under the tap. Returns true if a row
  // was hit (a tap on the title area returns false -> caller goes back).
  bool activateRowAtVY(int vy) {
    int idx = (vy - (LIST_TOP - 2)) / ROW_H;
    if (vy < LIST_TOP - 2 || idx < 0 || idx >= ROW_COUNT) return false;
    _sel = idx;
    activate((Row)idx);
    return true;
  }

private:
  bool rowState(Row row) const {
    switch (row) {
      case ROW_WIFI: return WiFi.getMode() != WIFI_OFF;
      case ROW_LORA: return _prefs->lora_enabled != 0;
      case ROW_GPS:  return _prefs->gps_enabled != 0;
      default:       return false;
    }
  }

  void activate(Row row) {
    switch (row) {
      case ROW_WIFI:    toggleWifi(); break;

      case ROW_LORA: {
        _prefs->lora_enabled = _prefs->lora_enabled ? 0 : 1;
        #ifdef MECK_RADIO_TOGGLES
        extern void loraRadioSetEnabled(bool enable);
        loraRadioSetEnabled(_prefs->lora_enabled != 0);
        #endif
        the_mesh.savePrefs();
        Serial.printf("SimpleSettings: LoRa mesh = %s\n",
                      _prefs->lora_enabled ? "ON" : "OFF");
        break;
      }

      case ROW_GPS:
        _task->toggleGPS();  // pref + hardware power + savePrefs + alert
        break;

      case ROW_FILEMGR: {
        // Jump into the stock settings screen and start the SD file manager
        // overlay (STA mode with on-screen IP, softAP fallback).
        _task->gotoSettingsScreen();
        SettingsScreen* ss = (SettingsScreen*)_task->getSettingsScreen();
        if (ss) ss->startFileMgr();
        break;
      }

      case ROW_ADVANCED:
        _task->gotoSettingsScreen();
        break;
    }
  }

  void toggleWifi() {
    // Same behaviour as the stock ROW_WIFI_TOGGLE (Settingsscreen.h)
    if (WiFi.getMode() != WIFI_OFF) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("SimpleSettings: WiFi radio OFF");
      return;
    }
    WiFi.mode(WIFI_STA);
    if (SD.exists("/web/wifi.cfg")) {
      File f = SD.open("/web/wifi.cfg", FILE_READ);
      String ssid, pass;
      if (f) {
        ssid = f.readStringUntil('\n'); ssid.trim();
        pass = f.readStringUntil('\n'); pass.trim();
        f.close();
      }
      digitalWrite(SDCARD_CS, HIGH);
      if (ssid.length() > 0) {
        WiFi.begin(ssid.c_str(), pass.c_str());
        unsigned long timeout = millis() + 8000;
        while (WiFi.status() != WL_CONNECTED && millis() < timeout) {
          yield();
          delay(100);
        }
        Serial.printf("SimpleSettings: WiFi ON, %s\n",
                      WiFi.status() == WL_CONNECTED ? "connected" : "connect failed");
      }
    } else {
      Serial.println("SimpleSettings: WiFi ON, no /web/wifi.cfg (use Advanced > WiFi Setup)");
    }
  }

  // Small switch graphic: 18x8 outline, 8x8 knob (left = off, right = on,
  // filled track when on).
  static void drawSwitch(DisplayDriver& display, int x, int y, bool on) {
    const int w = 18, h = 8;
    display.drawRect(x, y, w, h);
    if (on) {
      display.fillRect(x, y, w, h);                   // filled track = ON
      display.setColor(DisplayDriver::DARK);
      display.fillRect(x + w - 9, y + 1, 8, h - 2);   // dark knob right
      display.setColor(DisplayDriver::LIGHT);
    } else {
      display.fillRect(x + 1, y + 1, 8, h - 2);       // light knob left = OFF
    }
  }

  UITask* _task;
  NodePrefs* _prefs;
  int _sel;
};

#endif // MECK_SIMPLE_LAUNCHER
