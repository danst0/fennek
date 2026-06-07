#pragma once

// =============================================================================
// SimpleSettingsScreen — flat settings list (MECK_SIMPLE_LAUNCHER builds)
//
// Opened by the launcher's Settings entry. The full Meck settings moved one
// level down behind "Advanced >>".
//
//   WiFi          [switch]   — toggles the radio, joins the saved network
//   LoRa Mesh     [switch]   — SX1262 sleep + mesh loop pause, persisted
//   GPS           [switch]   — UITask::toggleGPS() (pref + hardware power)
//   WiFi Setup       >>      — scan/select/password flow (stock settings)
//   IP: DHCP|x.x.x.x         — optional static IP (empty = DHCP, the default)
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
#include "WifiConfig.h"
#include "Statusicons.h"

class SimpleSettingsScreen : public UIScreen {
public:
  static const int ROW_COUNT = 7;
  enum Row : uint8_t {
    ROW_WIFI, ROW_LORA, ROW_GPS,
    ROW_WIFI_SETUP, ROW_STATIC_IP,
    ROW_FILEMGR, ROW_ADVANCED,
  };

  // Layout (virtual 128x128): title at top, rows below, footer for WiFi info
  static const int LIST_TOP = 18;
  static const int ROW_H = 14;

  SimpleSettingsScreen(UITask* task, NodePrefs* prefs)
    : _task(task), _prefs(prefs), _sel(0), _editingIp(false) { _ipBuf[0] = '\0'; }

  int render(DisplayDriver& display) override {
    display.setColor(DisplayDriver::GREEN);
    display.setTextSize(_prefs->smallTextSize());
    display.setCursor(2, 2);
    display.print("Settings");
    display.setColor(DisplayDriver::LIGHT);

    // Title bar: WiFi + MeshCore status icons (top right, when active)
    meckDrawStatusIcons(display, _prefs);

    if (_editingIp) {
      renderIpEditor(display);
      return 1000;
    }

    for (int i = 0; i < ROW_COUNT; i++) {
      int y = LIST_TOP + i * ROW_H;

      // Selection marker: border around the active row
      if (i == _sel) {
        display.drawRect(0, y - 2, 128, ROW_H - 1);
      }

      display.setCursor(4, y + 1);
      switch ((Row)i) {
        case ROW_WIFI:       display.print("WiFi");         break;
        case ROW_LORA:       display.print("LoRa Mesh");    break;
        case ROW_GPS:        display.print("GPS");          break;
        case ROW_WIFI_SETUP: display.print("WiFi Setup");   break;
        case ROW_STATIC_IP: {
          String ip = currentStaticIp();
          String label = "IP: " + (ip.length() ? ip : String("DHCP"));
          display.print(label.c_str());
          break;
        }
        case ROW_FILEMGR:    display.print("File Manager"); break;
        case ROW_ADVANCED:   display.print("Advanced");     break;
      }

      if (i == ROW_WIFI || i == ROW_LORA || i == ROW_GPS) {
        drawSwitch(display, 104, y + 2, rowState((Row)i));
      } else if (i != ROW_STATIC_IP) {
        display.setCursor(110, y + 1);
        display.print(">>");
      }
    }

    // Footer: WiFi connection info
    display.setTextSize(0);
    if (WiFi.status() == WL_CONNECTED) {
      char buf[48];
      snprintf(buf, sizeof(buf), "%s  %s",
               WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      display.setColor(DisplayDriver::GREEN);
      display.setCursor(2, 119);
      display.print(buf);
      display.setColor(DisplayDriver::LIGHT);
    }
    display.setTextSize(1);

    return 2000;  // WiFi status may change in the background
  }

  bool handleInput(char c) override {
    if (_editingIp) return handleIpEditorInput(c);

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
  // While the IP editor is open, taps confirm (any row area) — keyboard
  // editing is the primary input there.
  bool activateRowAtVY(int vy) {
    if (_editingIp) return true;  // ignore taps while editing (keyboard flow)
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

  String currentStaticIp() const {
    String ssid, pass, staticIp;
    meckWifiReadConfig(ssid, pass, staticIp);
    return staticIp;
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

      case ROW_WIFI_SETUP: {
        #ifdef MECK_WIFI_COMPANION
        // Scan/select/password flow lives in the stock settings screen
        _task->gotoSettingsScreen();
        SettingsScreen* ss = (SettingsScreen*)_task->getSettingsScreen();
        if (ss) ss->startWifiSetup();
        #endif
        break;
      }

      case ROW_STATIC_IP: {
        // Open the inline IP editor pre-filled with the current value
        String ip = currentStaticIp();
        strncpy(_ipBuf, ip.c_str(), sizeof(_ipBuf) - 1);
        _ipBuf[sizeof(_ipBuf) - 1] = '\0';
        _editingIp = true;
        break;
      }

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
    if (WiFi.getMode() != WIFI_OFF) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("SimpleSettings: WiFi radio OFF");
      return;
    }
    // Connect using the saved config (DHCP or static IP)
    if (!meckWifiConnectSaved(8000)) {
      Serial.println("SimpleSettings: no saved WiFi or connect failed (WiFi Setup)");
    }
  }

  // ---- Inline static-IP editor (empty = DHCP) ----

  void renderIpEditor(DisplayDriver& display) {
    display.setCursor(4, 26);
    display.print("Static IP (empty = DHCP):");
    display.drawRect(2, 40, 124, 14);
    display.setCursor(6, 43);
    display.print(_ipBuf);
    // blinking-less cursor block
    int cx = 6 + display.getTextWidth(_ipBuf);
    display.fillRect(cx + 1, 43, 5, 8);
    display.setTextSize(0);
    display.setCursor(4, 62);
    display.print("Digits + '.'  Enter:Save");
    display.setCursor(4, 72);
    display.print("Backspace:Del  Q:Cancel");
    display.setCursor(4, 88);
    display.print("Gateway/DNS = x.x.x.1");
    display.setTextSize(1);
  }

  bool handleIpEditorInput(char c) {
    size_t len = strlen(_ipBuf);
    if ((c >= '0' && c <= '9') || c == '.') {
      if (len < sizeof(_ipBuf) - 1) {
        _ipBuf[len] = c;
        _ipBuf[len + 1] = '\0';
      }
      return true;
    }
    if (c == '\b' || c == 8 || c == 127) {
      if (len > 0) _ipBuf[len - 1] = '\0';
      return true;
    }
    if (c == KEY_ENTER || c == '\r') {
      String ip = String(_ipBuf);
      ip.trim();
      IPAddress parsed;
      if (ip.length() > 0 && !parsed.fromString(ip)) {
        Serial.printf("SimpleSettings: invalid IP '%s' — not saved\n", ip.c_str());
        return true;  // stay in editor
      }
      meckWifiSaveStaticIp(ip);  // "" = DHCP
      _editingIp = false;
      // Apply immediately when WiFi is currently on
      if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true);
        meckWifiConnectSaved(8000);
      }
      return true;
    }
    if (c == 'q' || c == KEY_CANCEL) {
      _editingIp = false;
      return true;
    }
    return true;  // consume everything else while editing
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
  bool _editingIp;
  char _ipBuf[20];
};

#endif // MECK_SIMPLE_LAUNCHER
