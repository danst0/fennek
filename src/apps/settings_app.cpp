// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "settings_app.h"
#include "mesh_client.h"
#include "config.h"
#include "core/battery.h"
#include "core/gui.h"
#include "core/settings.h"
#include "core/i18n.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <string.h>

namespace {

using gui::Rect;

// --- Layout -------------------------------------------------------------------
// Kein eigener Titel — die Statuszeile zeigt bereits "Einstellungen".
// Zwei Sektionen (Funk/System), darunter Home-Button + Info-Fußzeilen.
constexpr int W = EINK_W;
constexpr int TOP       = appmgr::CONTENT_Y;              // 24
constexpr int SEC_H     = 15;                             // Sektions-Überschrift
constexpr int ROW_H     = 20;                             // 11 Zeilen müssen passen
constexpr int NUM_FUNK  = 7;                              // ROW_PRESET..ROW_NAME
constexpr int FUNK_Y    = TOP + SEC_H;                    // 39
constexpr int SYS_HDR_Y = FUNK_Y + NUM_FUNK * ROW_H;      // 179
constexpr int SYS_Y     = SYS_HDR_Y + SEC_H;              // 194 (+4*20 = 274)
constexpr int VAL_RIGHT = W - 22;                         // Wert rechtsbündig, Platz für ►

const Rect kHome {6, 274, 110, 40};
constexpr int FOOT_X = 120;                               // Textspalte rechts vom Button

// --- Zeilen ---------------------------------------------------------------------
enum RowId {
  ROW_PRESET, ROW_FREQ, ROW_BW, ROW_SF, ROW_CR, ROW_TX, ROW_NAME,
  ROW_LANG, ROW_STANDBY, ROW_WSSID, ROW_WPASS,
  ROW_COUNT
};

// Auto-Standby-Stufen (Minuten; 0 = Aus).
const uint8_t kStandbySteps[] = {0, 2, 5, 10, 30};
constexpr int kNumStandby = 5;

// Per Tastatur editierbare Text-Zeilen (Enter startet, Enter speichert).
bool rowEditable(int row) {
  return row == ROW_NAME || row == ROW_WSSID || row == ROW_WPASS;
}

int  s_sel = 0;
int  s_edit = -1;          // gerade editierte Zeile (-1 = keine)
char s_editBuf[65] = "";   // groß genug fürs WLAN-Passwort (64)

// Presets (Quelle: MeshCore-Community; Narrow = Standard in DE/NRW,
// identisch zu Daniels Repeater: 869,618 / 62,5 / SF8 / CR4-5).
struct Preset { const char* name; settings::MeshParams p; };
const Preset kPresets[] = {
  {"EU Narrow",    {869.618f, 62.5f,  8, 5, 22}},
  {"EU Klassisch", {869.525f, 250.0f, 11, 5, 22}},
};
constexpr int kNumPresets = 2;

void markDirty() { appmgr::markDirty(); }

// Passt der aktuelle Parametersatz zu einem Preset? (-1 = manuell)
int matchPreset(const settings::MeshParams& p) {
  for (int i = 0; i < kNumPresets; i++) {
    const settings::MeshParams& q = kPresets[i].p;
    if (p.freqMhz == q.freqMhz && p.bwKhz == q.bwKhz && p.sf == q.sf && p.cr == q.cr)
      return i;
  }
  return -1;
}

// Parameter speichern + live anwenden.
void apply(const settings::MeshParams& p) {
  settings::setMeshParams(p);
  mesh_client::applyRadioParams();
  markDirty();
}

// Bandbreiten-Stufen (LoRa-üblich).
float nextBw(float bw, int dir) {
  const float steps[] = {62.5f, 125.0f, 250.0f, 500.0f};
  int idx = 0;
  for (int i = 0; i < 4; i++) if (bw == steps[i]) idx = i;
  idx += dir;
  if (idx < 0) idx = 3;
  if (idx > 3) idx = 0;
  return steps[idx];
}

// Wert der Zeile ändern (dir = -1/+1).
void changeRow(int row, int dir) {
  if (row == ROW_LANG) {
    int n = (int)i18n::Lang::COUNT;
    int l = ((int)i18n::lang() + dir + n) % n;
    i18n::setLang((i18n::Lang)l);
    markDirty();
    return;
  }
  if (row == ROW_STANDBY) {
    uint8_t cur = settings::standbyMinutes();
    int idx = 0;
    for (int i = 0; i < kNumStandby; i++) if (kStandbySteps[i] == cur) idx = i;
    idx = (idx + dir + kNumStandby) % kNumStandby;
    settings::setStandbyMinutes(kStandbySteps[idx]);
    markDirty();
    return;
  }
  settings::MeshParams p = settings::meshParams();
  switch (row) {
    case ROW_PRESET: {
      int cur = matchPreset(p);
      int next = (cur < 0) ? (dir > 0 ? 0 : kNumPresets - 1)
                           : (cur + dir + kNumPresets) % kNumPresets;
      settings::MeshParams np = kPresets[next].p;
      np.txDbm = p.txDbm;          // Sendeleistung bleibt persönliche Wahl
      apply(np);
      return;
    }
    case ROW_FREQ:
      p.freqMhz += dir * 0.025f;
      if (p.freqMhz < 863.0f) p.freqMhz = 863.0f;
      if (p.freqMhz > 870.0f) p.freqMhz = 870.0f;
      break;
    case ROW_BW:  p.bwKhz = nextBw(p.bwKhz, dir); break;
    case ROW_SF:
      p.sf = (uint8_t)constrain((int)p.sf + dir, 7, 12);
      break;
    case ROW_CR:
      p.cr = (uint8_t)constrain((int)p.cr + dir, 5, 8);
      break;
    case ROW_TX:
      p.txDbm = (uint8_t)constrain((int)p.txDbm + dir, 1, 22);
      break;
    default: return;   // Text-Zeilen (Name/WLAN) werden per Enter editiert
  }
  apply(p);
}

const char* rowName(int row) {
  using i18n::Str;
  switch (row) {
    case ROW_PRESET:  return "Preset";       // LoRa-Jargon, nicht übersetzt
    case ROW_FREQ:    return i18n::tr(Str::LblFreq);
    case ROW_BW:      return i18n::tr(Str::LblBandwidth);
    case ROW_SF:      return "Spreading";    // LoRa-Jargon
    case ROW_CR:      return i18n::tr(Str::LblCodingRate);
    case ROW_TX:      return i18n::tr(Str::LblTxPower);
    case ROW_NAME:    return i18n::tr(Str::LblName);
    case ROW_LANG:    return i18n::tr(Str::SettingsLang);
    case ROW_STANDBY: return i18n::tr(Str::LblStandby);
    case ROW_WSSID:   return i18n::tr(Str::LblWifiSsid);
    case ROW_WPASS:   return i18n::tr(Str::LblWifiPass);
  }
  return "";
}

void rowValue(int row, char* v, size_t n) {
  v[0] = '\0';
  settings::MeshParams p = settings::meshParams();
  switch (row) {
    case ROW_PRESET: {
      int m = matchPreset(p);
      snprintf(v, n, "%s", m >= 0 ? kPresets[m].name
                                  : i18n::tr(i18n::Str::SettingsManual));
      break;
    }
    case ROW_FREQ: snprintf(v, n, "%.3f MHz", (double)p.freqMhz); break;
    case ROW_BW:   snprintf(v, n, "%.1f kHz", (double)p.bwKhz); break;
    case ROW_SF:   snprintf(v, n, "SF%u", p.sf); break;
    case ROW_CR:   snprintf(v, n, "4/%u", p.cr); break;
    case ROW_TX:   snprintf(v, n, "%u dBm", p.txDbm); break;
    case ROW_NAME:
      if (s_edit == ROW_NAME) snprintf(v, n, "%s_", s_editBuf);
      else                    settings::meshName(v, n);
      break;
    case ROW_LANG:
      snprintf(v, n, "%s", i18n::langName(i18n::lang()));
      break;
    case ROW_STANDBY: {
      uint8_t m = settings::standbyMinutes();
      if (m == 0) snprintf(v, n, "%s", i18n::tr(i18n::Str::StandbyOff));
      else        snprintf(v, n, "%u min", m);
      break;
    }
    case ROW_WSSID:
      if (s_edit == ROW_WSSID) snprintf(v, n, "%s_", s_editBuf);
      else {
        settings::wifiSsid(v, n);
        if (!v[0]) snprintf(v, n, "-");
      }
      break;
    case ROW_WPASS:
      // Klartext nur während der Eingabe; sonst nur "gesetzt"-Indikator.
      if (s_edit == ROW_WPASS) snprintf(v, n, "%s_", s_editBuf);
      else {
        char pw[65];
        settings::wifiPass(pw, sizeof(pw));
        snprintf(v, n, "%s", pw[0] ? "****" : "-");
      }
      break;
  }
}

int rowY(int row) {
  return (row < NUM_FUNK) ? FUNK_Y + row * ROW_H
                          : SYS_Y + (row - NUM_FUNK) * ROW_H;
}

// Welche Zeile liegt unter (x,y)? -1 = keine.
int hitRow(int y) {
  if (y >= FUNK_Y && y < FUNK_Y + NUM_FUNK * ROW_H) return (y - FUNK_Y) / ROW_H;
  if (y >= SYS_Y && y < SYS_Y + (ROW_COUNT - NUM_FUNK) * ROW_H)
    return NUM_FUNK + (y - SYS_Y) / ROW_H;
  return -1;
}

void sectionHeader(Adafruit_GFX& g, int y, const char* title) {
  g.setTextSize(1);
  g.setCursor(8, y + 3);
  gui::print(g, title);
  g.drawFastHLine(0, y + SEC_H - 1, W, GxEPD_BLACK);
}

// Zeile: Label klein links, Wert groß rechtsbündig; ausgewählte Zeile mit
// Doppelrahmen + ◄/►-Pfeilen als Hinweis auf die Tap-Hälften bzw. A/D.
void drawRow(Adafruit_GFX& g, int row) {
  const int y = rowY(row);

  g.setTextSize(1);
  uint16_t lw, lh;
  gui::textBounds(g, rowName(row), &lw, &lh);
  g.setCursor(10, y + 8);
  gui::print(g, rowName(row));

  char v[40];
  rowValue(row, v, sizeof(v));
  g.setTextSize(2);
  uint16_t vw, vh;
  gui::textBounds(g, v, &vw, &vh);
  int vx = VAL_RIGHT - (int)vw;
  int vy = y + 5;
  if (vx < 10 + (int)lw + 8) {           // zu lang (z. B. langer Name): klein rendern
    g.setTextSize(1);
    gui::textBounds(g, v, &vw, &vh);
    vx = VAL_RIGHT - (int)vw;
    if (vx < 10 + (int)lw + 8) vx = 10 + (int)lw + 8;
    vy = y + 8;
  }
  g.setCursor(vx, vy);
  gui::print(g, v);

  g.drawFastHLine(0, y + ROW_H, W, GxEPD_BLACK);

  if (row == s_sel) {
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
    if (!rowEditable(row)) {             // Text-Zeilen werden per Enter editiert
      g.setTextSize(1);
      g.setCursor(vx - 15, y + 8);
      g.write((uint8_t)0x11);            // ◄
      g.setCursor(W - 13, y + 8);
      g.write((uint8_t)0x10);            // ►
    }
  }
}

void startEdit(int row) {
  s_editBuf[0] = '\0';
  switch (row) {
    case ROW_NAME:  settings::meshName(s_editBuf, sizeof(s_editBuf)); break;
    case ROW_WSSID: settings::wifiSsid(s_editBuf, sizeof(s_editBuf)); break;
    case ROW_WPASS: settings::wifiPass(s_editBuf, sizeof(s_editBuf)); break;
    default: return;
  }
  s_edit = row;
  markDirty();
}

void finishEdit(bool save) {
  if (save) {
    switch (s_edit) {
      // Leerer Node-Name wird verworfen; leere WLAN-Werte löschen den Eintrag.
      case ROW_NAME:  if (s_editBuf[0]) mesh_client::setNodeName(s_editBuf); break;
      case ROW_WSSID: settings::setWifiSsid(s_editBuf); break;
      case ROW_WPASS: settings::setWifiPass(s_editBuf); break;
    }
  }
  s_edit = -1;
  markDirty();
}

void onKey(char k) {
  // Editiermodus: Tasten gehen in den Text (Name/SSID/Passwort).
  if (s_edit >= 0) {
    if (k == '\r')      { finishEdit(true); return; }
    if (k == 0x02)      { finishEdit(false); return; }   // Mic = abbrechen
    if (k == '\b') {
      int len = strlen(s_editBuf);
      if (len > 0) { s_editBuf[len - 1] = '\0'; markDirty(); }
      else finishEdit(false);
      return;
    }
    // Eingabelimit: Node-Name/SSID 32 Zeichen, Passwort 64.
    int maxLen = (s_edit == ROW_WPASS) ? 64 : 32;
    if (k >= 32 && k < 127) {
      int len = strlen(s_editBuf);
      if (len < maxLen && len < (int)sizeof(s_editBuf) - 1) {
        s_editBuf[len] = k;
        s_editBuf[len + 1] = '\0';
        markDirty();
      }
    }
    return;
  }

  switch (k) {
    case 'w': case 'W': s_sel = (s_sel + ROW_COUNT - 1) % ROW_COUNT; markDirty(); break;
    case 's': case 'S': s_sel = (s_sel + 1) % ROW_COUNT; markDirty(); break;
    case 'a': case 'A': changeRow(s_sel, -1); break;
    case 'd': case 'D': changeRow(s_sel, +1); break;
    case '\r':
      if (rowEditable(s_sel)) startEdit(s_sel);
      else changeRow(s_sel, +1);
      break;
    case '\b':
    case 'q': case 'Q': appmgr::goHome(); break;
    default: break;
  }
}

void onTouch(int x, int y) {
  if (kHome.hit(x, y)) {
    if (s_edit >= 0) finishEdit(true);
    appmgr::goHome();
    return;
  }
  int row = hitRow(y);
  if (row < 0) return;
  // Erster Tap wählt nur aus (verhindert versehentliches Verstellen),
  // Tap auf die ausgewählte Zeile ändert: linke Hälfte -, rechte +.
  if (row != s_sel) { s_sel = row; markDirty(); return; }
  if (rowEditable(row)) { startEdit(row); return; }
  changeRow(row, (x >= W / 2) ? +1 : -1);
}

class SettingsApp : public App {
 public:
  const char* id()   const override { return "Einstellungen"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppSettings); }

  void onLeave() override {
    if (s_edit >= 0) finishEdit(true);
  }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::TAP) onTouch(e.x, e.y);
    else                           onKey(e.key);
  }

  void draw(Adafruit_GFX& g) override {
    using i18n::Str;
    g.setTextColor(GxEPD_BLACK);

    sectionHeader(g, TOP, i18n::tr(Str::SecRadio));
    for (int r = 0; r < NUM_FUNK; r++) drawRow(g, r);

    sectionHeader(g, SYS_HDR_Y, i18n::tr(Str::SecSystem));
    for (int r = NUM_FUNK; r < ROW_COUNT; r++) drawRow(g, r);

    gui::drawButton(g, kHome, i18n::tr(Str::BtnHome), false);
    g.setTextSize(1);
    g.setCursor(FOOT_X, 278);
    if (s_edit >= 0)              gui::print(g, i18n::tr(Str::HintEnterSave));
    else if (s_sel == ROW_NAME)   gui::print(g, i18n::tr(Str::HintNameEdit));
    else if (rowEditable(s_sel))  gui::print(g, i18n::tr(Str::HintEdit));
    else                          gui::print(g, i18n::tr(Str::HintChange));
    char info[32];
    snprintf(info, sizeof(info), i18n::tr(Str::FmtBattery), battery::percent(),
             battery::charging() ? "+" : "", battery::milliVolts());
    g.setCursor(FOOT_X, 290);
    gui::print(g, info);
    g.setCursor(FOOT_X, 302);
    gui::print(g, "Fennek " FENNEK_VERSION);
    g.setCursor(FOOT_X, 314);
    gui::print(g, "(c) Dr. Daniel Dumke");
  }
};

SettingsApp s_app;

}  // namespace

namespace settings_app {

App* get() { return &s_app; }

}  // namespace settings_app
