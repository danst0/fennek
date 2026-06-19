// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "settings_app.h"
#include "mesh_client.h"
#include "config.h"
#include "core/battery.h"
#include "core/gui.h"
#include "core/settings.h"
#include "core/i18n.h"
#include "services/timesync.h"

#include <Arduino.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <string.h>
#include <time.h>

namespace {

using gui::Rect;

// --- Layout -------------------------------------------------------------------
// Zwei-Ebenen-Navigation: die Wurzel zeigt Kategorien ("Ordner"), die Auswahl
// öffnet die zugehörigen Zeilen. So bleibt jede Ebene kurz genug fürs E-Ink und
// neue Bereiche (Navidrome …) überfüllen die Liste nicht. Kein Titel — die
// Statuszeile zeigt bereits "Einstellungen".
constexpr int W = EINK_W;
constexpr int TOP       = appmgr::CONTENT_Y;              // 24
constexpr int SEC_H     = 15;                             // Kategorie-Kopfzeile
constexpr int ROW_H     = 20;
constexpr int VP_TOP    = TOP;                            // Inhalt oben
constexpr int VAL_RIGHT = W - 22;                         // Wert rechtsbündig, Platz für ►

const Rect kHome {6, 274, 110, 40};
constexpr int FOOT_X = 120;                               // Textspalte rechts vom Button

// --- Zeilen ---------------------------------------------------------------------
enum RowId {
  ROW_PRESET, ROW_FREQ, ROW_BW, ROW_SF, ROW_CR, ROW_TX, ROW_NAME,
  ROW_LANG, ROW_STANDBY, ROW_FONT, ROW_TZ, ROW_TIME, ROW_WSSID, ROW_WPASS,
  ROW_NAVON, ROW_NAVURL, ROW_NAVUSER, ROW_NAVPASS,
  ROW_AION, ROW_AIURL, ROW_AIMODEL,
  ROW_COUNT
};

// --- Kategorien ("Ordner") ------------------------------------------------------
enum { CAT_RADIO, CAT_SYSTEM, CAT_TIME, CAT_WIFI, CAT_NAV, CAT_AI, CAT_COUNT };

const RowId kRadioRows[]  = {ROW_PRESET, ROW_FREQ, ROW_BW, ROW_SF, ROW_CR, ROW_TX, ROW_NAME};
const RowId kSystemRows[] = {ROW_LANG, ROW_STANDBY, ROW_FONT};
const RowId kTimeRows[]   = {ROW_TZ, ROW_TIME};
const RowId kWifiRows[]   = {ROW_WSSID, ROW_WPASS};
const RowId kNavRows[]    = {ROW_NAVON, ROW_NAVURL, ROW_NAVUSER, ROW_NAVPASS};
const RowId kAiRows[]     = {ROW_AION, ROW_AIURL, ROW_AIMODEL};

struct CatRows { const RowId* rows; int count; };
const CatRows kCats[] = {
  {kRadioRows,  (int)(sizeof(kRadioRows)  / sizeof(RowId))},
  {kSystemRows, (int)(sizeof(kSystemRows) / sizeof(RowId))},
  {kTimeRows,   (int)(sizeof(kTimeRows)   / sizeof(RowId))},
  {kWifiRows,   (int)(sizeof(kWifiRows)   / sizeof(RowId))},
  {kNavRows,    (int)(sizeof(kNavRows)    / sizeof(RowId))},
  {kAiRows,     (int)(sizeof(kAiRows)     / sizeof(RowId))},
};

const char* catName(int c) {
  switch (c) {
    case CAT_RADIO:  return i18n::tr(i18n::Str::SecRadio);
    case CAT_SYSTEM: return i18n::tr(i18n::Str::SecSystem);
    case CAT_TIME:   return "Zeit";
    case CAT_WIFI:   return "WLAN";
    case CAT_NAV:    return "Navidrome";
    case CAT_AI:     return "Ollama";
  }
  return "";
}

int  s_cat  = -1;          // -1 = Kategorie-Liste (Wurzel); sonst CAT_*
int  s_sel  = 0;           // Index in der aktuellen Ebene (Kategorie bzw. Zeile)
int  s_edit = -1;          // gerade editierte Zeile (RowId; -1 = keine)
char s_editBuf[128] = "";  // groß genug für die Navidrome-URL (127)

// Per Tastatur editierbare Text-Zeilen (Enter startet, Enter speichert).
bool rowEditable(int row) {
  return row == ROW_NAME || row == ROW_WSSID || row == ROW_WPASS ||
         row == ROW_TIME || row == ROW_NAVURL || row == ROW_NAVUSER ||
         row == ROW_NAVPASS || row == ROW_AIURL || row == ROW_AIMODEL;
}

// Auto-Standby-Stufen (Minuten; 0 = Aus).
const uint8_t kStandbySteps[] = {0, 2, 5, 10, 30};
constexpr int kNumStandby = 5;

// Zeitzonen-Presets (POSIX-TZ inkl. automatischer Sommerzeit). NTP/Mesh liefern
// nur UTC; die Zone ist eine reine Anzeige-Einstellung.
struct TzPreset { const char* name; const char* tz; };
const TzPreset kTz[] = {
  {"Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"London", "GMT0BST,M3.5.0/1,M10.5.0"},
  {"UTC",    "UTC0"},
  {"Athen",  "EET-2EEST,M3.5.0/3,M10.5.0/4"},
};
constexpr int kNumTz = (int)(sizeof(kTz) / sizeof(kTz[0]));

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
  if (row == ROW_TZ) {
    char cur[48];
    settings::tzString(cur, sizeof(cur));
    int idx = 0;
    for (int i = 0; i < kNumTz; i++) if (strcmp(kTz[i].tz, cur) == 0) idx = i;
    idx = (idx + dir + kNumTz) % kNumTz;
    settings::setTzString(kTz[idx].tz);
    timesync::applyTimezone();
    markDirty();
    return;
  }
  if (row == ROW_FONT) {
    // Nur zwei Stufen (klein/groß) — jede Richtung toggelt.
    settings::setFontScale(settings::fontScale() >= 2 ? 1 : 2);
    markDirty();
    return;
  }
  if (row == ROW_NAVON) {
    settings::setNavEnabled(!settings::navEnabled());
    markDirty();
    return;
  }
  if (row == ROW_AION) {
    settings::setAiEnabled(!settings::aiEnabled());
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
    default: return;   // Text-Zeilen (Name/WLAN/Navidrome) per Enter editieren
  }
  apply(p);
}

const char* rowName(int row) {
  using i18n::Str;
  switch (row) {
    case ROW_PRESET:   return "Preset";       // LoRa-Jargon, nicht übersetzt
    case ROW_FREQ:     return i18n::tr(Str::LblFreq);
    case ROW_BW:       return i18n::tr(Str::LblBandwidth);
    case ROW_SF:       return "Spreading";    // LoRa-Jargon
    case ROW_CR:       return i18n::tr(Str::LblCodingRate);
    case ROW_TX:       return i18n::tr(Str::LblTxPower);
    case ROW_NAME:     return i18n::tr(Str::LblName);
    case ROW_LANG:     return i18n::tr(Str::SettingsLang);
    case ROW_STANDBY:  return i18n::tr(Str::LblStandby);
    case ROW_FONT:     return i18n::tr(Str::LblFontSize);
    case ROW_TZ:       return "Zeitzone";
    case ROW_TIME:     return "Zeit";
    case ROW_WSSID:    return i18n::tr(Str::LblWifiSsid);
    case ROW_WPASS:    return i18n::tr(Str::LblWifiPass);
    case ROW_NAVON:    return "Scrobbeln";
    case ROW_NAVURL:   return "Server";
    case ROW_NAVUSER:  return "Benutzer";
    case ROW_NAVPASS:  return "Passwort";
    case ROW_AION:     return "Schoenschreiben";
    case ROW_AIURL:    return "Server";
    case ROW_AIMODEL:  return "Modell";
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
    case ROW_FONT:
      snprintf(v, n, "%s", settings::fontScale() >= 2 ? i18n::tr(i18n::Str::FontLarge)
                                                      : i18n::tr(i18n::Str::FontSmall));
      break;
    case ROW_TZ: {
      char tz[48];
      settings::tzString(tz, sizeof(tz));
      int idx = -1;
      for (int i = 0; i < kNumTz; i++) if (strcmp(kTz[i].tz, tz) == 0) idx = i;
      snprintf(v, n, "%s", idx >= 0 ? kTz[idx].name : "eigen");
      break;
    }
    case ROW_TIME:
      // Beim Editieren der Eingabepuffer; sonst die aktuelle lokale Uhrzeit.
      if (s_edit == ROW_TIME) snprintf(v, n, "%s_", s_editBuf);
      else {
        time_t    tt = (time_t)timesync::now();
        struct tm lt;
        localtime_r(&tt, &lt);
        snprintf(v, n, "%02u:%02u", (unsigned)lt.tm_hour, (unsigned)lt.tm_min);
      }
      break;
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
    case ROW_NAVON:
      snprintf(v, n, "%s", settings::navEnabled() ? "An" : "Aus");
      break;
    case ROW_NAVURL:
      if (s_edit == ROW_NAVURL) snprintf(v, n, "%s_", s_editBuf);
      else {
        settings::navUrl(v, n);
        if (!v[0]) snprintf(v, n, "-");
      }
      break;
    case ROW_NAVUSER:
      if (s_edit == ROW_NAVUSER) snprintf(v, n, "%s_", s_editBuf);
      else {
        settings::navUser(v, n);
        if (!v[0]) snprintf(v, n, "-");
      }
      break;
    case ROW_NAVPASS:
      if (s_edit == ROW_NAVPASS) snprintf(v, n, "%s_", s_editBuf);
      else {
        char pw[65];
        settings::navPass(pw, sizeof(pw));
        snprintf(v, n, "%s", pw[0] ? "****" : "-");
      }
      break;
    case ROW_AION:
      snprintf(v, n, "%s", settings::aiEnabled() ? "An" : "Aus");
      break;
    case ROW_AIURL:
      if (s_edit == ROW_AIURL) snprintf(v, n, "%s_", s_editBuf);
      else {
        settings::aiUrl(v, n);
        if (!v[0]) snprintf(v, n, "-");
      }
      break;
    case ROW_AIMODEL:
      if (s_edit == ROW_AIMODEL) snprintf(v, n, "%s_", s_editBuf);
      else settings::aiModel(v, n);
      break;
  }
}

// --- Treffer-Zonen (Touch) ------------------------------------------------------
// Welche Kategorie liegt unter dem Tap bei y (Wurzelansicht)? -1 = keine.
int hitCat(int ty) {
  int y = VP_TOP;
  for (int i = 0; i < CAT_COUNT; i++) {
    if (ty >= y && ty < y + ROW_H) return i;
    y += ROW_H;
  }
  return -1;
}

// Welcher Zeilen-Index (innerhalb der Kategorie) liegt unter dem Tap? -1 = keiner.
int hitRow(int ty) {
  int y = VP_TOP + SEC_H;   // Zeilen beginnen unter der Kopfzeile
  for (int i = 0; i < kCats[s_cat].count; i++) {
    if (ty >= y && ty < y + ROW_H) return i;
    y += ROW_H;
  }
  return -1;
}

// --- Zeichnen -------------------------------------------------------------------
// Kategorie-Zeile (Wurzel): Name groß + ►; ausgewählt mit Doppelrahmen.
void drawCatRow(Adafruit_GFX& g, int idx, int y) {
  g.setTextSize(2);
  g.setCursor(10, y + 4);
  gui::print(g, catName(idx));
  g.drawFastHLine(0, y + ROW_H, W, GxEPD_BLACK);
  g.setTextSize(1);
  g.setCursor(W - 13, y + 8);
  g.write((uint8_t)0x10);                // ►
  if (idx == s_sel) {
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
  }
}

// Einstellungs-Zeile: Label klein links, Wert groß rechtsbündig; ausgewählte
// Zeile mit Doppelrahmen + ◄/►-Pfeilen (Tap-Hälften bzw. A/D).
void drawRow(Adafruit_GFX& g, int row, int y, bool selected) {
  g.setTextSize(1);
  uint16_t lw, lh;
  gui::textBounds(g, rowName(row), &lw, &lh);
  g.setCursor(10, y + 8);
  gui::print(g, rowName(row));

  char v[64];
  rowValue(row, v, sizeof(v));
  g.setTextSize(2);
  uint16_t vw, vh;
  gui::textBounds(g, v, &vw, &vh);
  int vx = VAL_RIGHT - (int)vw;
  int vy = y + 5;
  if (vx < 10 + (int)lw + 8) {           // zu lang (z. B. URL/Name): klein rendern
    g.setTextSize(1);
    gui::textBounds(g, v, &vw, &vh);
    vx = VAL_RIGHT - (int)vw;
    if (vx < 10 + (int)lw + 8) vx = 10 + (int)lw + 8;
    vy = y + 8;
  }
  g.setCursor(vx, vy);
  gui::print(g, v);

  g.drawFastHLine(0, y + ROW_H, W, GxEPD_BLACK);

  if (selected) {
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
    case ROW_NAME:    settings::meshName(s_editBuf, sizeof(s_editBuf)); break;
    case ROW_WSSID:   settings::wifiSsid(s_editBuf, sizeof(s_editBuf)); break;
    case ROW_WPASS:   settings::wifiPass(s_editBuf, sizeof(s_editBuf)); break;
    case ROW_NAVURL:  settings::navUrl(s_editBuf, sizeof(s_editBuf)); break;
    case ROW_NAVUSER: settings::navUser(s_editBuf, sizeof(s_editBuf)); break;
    case ROW_NAVPASS: settings::navPass(s_editBuf, sizeof(s_editBuf)); break;
    case ROW_AIURL:   settings::aiUrl(s_editBuf, sizeof(s_editBuf)); break;
    case ROW_AIMODEL: settings::aiModel(s_editBuf, sizeof(s_editBuf)); break;
    case ROW_TIME: {
      // Mit der aktuellen lokalen Zeit als Vorlage vorbelegen.
      time_t    tt = (time_t)timesync::now();
      struct tm lt;
      localtime_r(&tt, &lt);
      snprintf(s_editBuf, sizeof(s_editBuf), "%04d-%02d-%02d %02d:%02d",
               lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
      break;
    }
    default: return;
  }
  s_edit = row;
  markDirty();
}

void finishEdit(bool save) {
  if (save) {
    switch (s_edit) {
      // Leerer Node-Name wird verworfen; leere WLAN-/Navidrome-Werte löschen.
      case ROW_NAME:    if (s_editBuf[0]) mesh_client::setNodeName(s_editBuf); break;
      case ROW_WSSID:   settings::setWifiSsid(s_editBuf); break;
      case ROW_WPASS:   settings::setWifiPass(s_editBuf); break;
      case ROW_NAVURL:  settings::setNavUrl(s_editBuf); break;
      case ROW_NAVUSER: settings::setNavUser(s_editBuf); break;
      case ROW_NAVPASS: settings::setNavPass(s_editBuf); break;
      case ROW_AIURL:   settings::setAiUrl(s_editBuf); break;
      case ROW_AIMODEL: settings::setAiModel(s_editBuf); break;
      case ROW_TIME: {
        // "YYYY-MM-DD HH:MM" als lokale Zeit lesen → über die Zeitzone nach UTC.
        struct tm lt;
        memset(&lt, 0, sizeof(lt));
        int y, mo, d, h, mi;
        if (sscanf(s_editBuf, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
          lt.tm_year = y - 1900; lt.tm_mon = mo - 1; lt.tm_mday = d;
          lt.tm_hour = h; lt.tm_min = mi; lt.tm_isdst = -1;
          time_t utc = mktime(&lt);
          if (utc > 0) timesync::setManualTime((uint32_t)utc);
        }
        break;
      }
    }
  }
  s_edit = -1;
  markDirty();
}

// Aktuell ausgewählte Zeile (RowId) innerhalb der geöffneten Kategorie; -1 Wurzel.
int curRow() { return s_cat < 0 ? -1 : kCats[s_cat].rows[s_sel]; }

void onKey(char k) {
  // Editiermodus: Tasten gehen in den Text (Name/SSID/Passwort/URL …).
  if (s_edit >= 0) {
    if (k == '\r')      { finishEdit(true); return; }
    if (k == 0x02)      { finishEdit(false); return; }   // Mic = abbrechen
    if (k == '\b') {
      int len = strlen(s_editBuf);
      if (len > 0) { s_editBuf[len - 1] = '\0'; markDirty(); }
      else finishEdit(false);
      return;
    }
    // Eingabelimits je Feld: URL 127, Passwort 64, Benutzer 63, sonst 32.
    int maxLen;
    switch (s_edit) {
      case ROW_NAVURL:  maxLen = 127; break;
      case ROW_WPASS:
      case ROW_NAVPASS: maxLen = 64;  break;
      case ROW_NAVUSER: maxLen = 63;  break;
      default:          maxLen = 32;  break;
    }
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

  // Wurzelebene: Kategorien durchblättern, Enter/► öffnet.
  if (s_cat < 0) {
    switch (k) {
      case 'w': case 'W': s_sel = (s_sel + CAT_COUNT - 1) % CAT_COUNT; markDirty(); break;
      case 's': case 'S': s_sel = (s_sel + 1) % CAT_COUNT; markDirty(); break;
      case '\r': case 'd': case 'D': s_cat = s_sel; s_sel = 0; markDirty(); break;
      case '\b': case 'q': case 'Q': appmgr::goHome(); break;
      default: break;
    }
    return;
  }

  // Innerhalb einer Kategorie.
  int n   = kCats[s_cat].count;
  int row = kCats[s_cat].rows[s_sel];
  switch (k) {
    case 'w': case 'W': s_sel = (s_sel + n - 1) % n; markDirty(); break;
    case 's': case 'S': s_sel = (s_sel + 1) % n; markDirty(); break;
    case 'a': case 'A': changeRow(row, -1); break;
    case 'd': case 'D': changeRow(row, +1); break;
    case '\r':
      if (rowEditable(row)) startEdit(row);
      else changeRow(row, +1);
      break;
    case '\b': case 'q': case 'Q':   // zurück zur Kategorie-Liste
      s_sel = s_cat; s_cat = -1; markDirty(); break;
    default: break;
  }
}

void onTouch(int x, int y) {
  // Unterer Eckknopf = „Zurück" (genau eine Ebene): Bearbeitung verlassen →
  // Kategorie-Liste → Launcher. Home global über die Statuszeile.
  if (kHome.hit(x, y)) {
    if (s_edit >= 0) { finishEdit(true); return; }
    if (s_cat >= 0) { s_sel = s_cat; s_cat = -1; markDirty(); return; }
    appmgr::goHome();
    return;
  }

  // Wurzelebene: Tap auf eine Kategorie öffnet sie.
  if (s_cat < 0) {
    int c = hitCat(y);
    if (c >= 0) { s_cat = c; s_sel = 0; markDirty(); }
    return;
  }

  // Tap auf die Kopfzeile geht zurück zur Kategorie-Liste.
  if (y >= VP_TOP && y < VP_TOP + SEC_H) {
    s_sel = s_cat; s_cat = -1; markDirty();
    return;
  }

  int sidx = hitRow(y);
  if (sidx < 0) return;
  int row = kCats[s_cat].rows[sidx];
  // Erster Tap wählt nur aus; Tap auf die ausgewählte Zeile ändert/öffnet.
  if (sidx != s_sel) { s_sel = sidx; markDirty(); return; }
  if (rowEditable(row)) { startEdit(row); return; }
  changeRow(row, (x >= W / 2) ? +1 : -1);
}

class SettingsApp : public App {
 public:
  const char* id()   const override { return "Einstellungen"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppSettings); }

  void onEnter() override { s_cat = -1; s_sel = 0; s_edit = -1; }

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

    if (s_cat < 0) {
      // Wurzel: Kategorie-Liste.
      int y = VP_TOP;
      for (int i = 0; i < CAT_COUNT; i++) { drawCatRow(g, i, y); y += ROW_H; }
    } else {
      // Kopfzeile mit Zurück-Hinweis (◄), darunter die Zeilen.
      g.setTextSize(1);
      char hdr[40];
      snprintf(hdr, sizeof(hdr), "\x11 %s", catName(s_cat));   // 0x11 = ◄
      g.setCursor(8, VP_TOP + 3);
      gui::print(g, hdr);
      g.drawFastHLine(0, VP_TOP + SEC_H - 1, W, GxEPD_BLACK);
      int y = VP_TOP + SEC_H;
      for (int i = 0; i < kCats[s_cat].count; i++) {
        drawRow(g, kCats[s_cat].rows[i], y, i == s_sel);
        y += ROW_H;
      }
    }

    gui::drawButton(g, kHome, i18n::tr(Str::BtnBack), false);
    g.setTextSize(1);
    g.setCursor(FOOT_X, 278);
    if (s_edit >= 0)                gui::print(g, i18n::tr(Str::HintEnterSave));
    else if (s_cat < 0)            gui::print(g, "Ordner oeffnen");
    else if (curRow() == ROW_NAME) gui::print(g, i18n::tr(Str::HintNameEdit));
    else if (rowEditable(curRow())) gui::print(g, i18n::tr(Str::HintEdit));
    else                           gui::print(g, i18n::tr(Str::HintChange));
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
