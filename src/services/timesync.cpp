// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "timesync.h"
#include "apps/mesh_client.h"
#include "services/webfm.h"
#include "core/settings.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>

namespace {

// Untergrenze für plausible Echtzeit (≈ 09.06.2026, Build-Ära) — alles davor ist
// Kaltstart-Müll (ESP32 startet ohne gesetzte Zeit ~1970).
constexpr uint32_t kSaneEpoch       = 1781000000UL;
constexpr uint32_t kStaleSeconds    = 300;        // ~5 min ⇒ "schlecht"
constexpr uint16_t kDefaultPpm      = 2000;       // konservativ, bis gemessen
constexpr uint16_t kMinPpm          = 5;
constexpr uint16_t kMaxPpm          = 60000;      // 6 %
constexpr uint32_t kMinLearnSecs    = 600;        // EMA nur über ≥10-min-Intervalle
constexpr uint32_t kNvsSaveMs       = 600000;     // 10 min
constexpr uint32_t kNtpTimeoutMs    = 10000;      // opportunistisch
constexpr uint32_t kConnectTimeoutMs= 8000;       // Pre-Standby-Connect
constexpr uint32_t kNtpWaitMs       = 6000;       // Pre-Standby-NTP

const char* NTP1 = "de.pool.ntp.org";
const char* NTP2 = "pool.ntp.org";

// Exponentielles Back-off für den selbst-initiierten Pre-Standby-Sync, damit ein
// dauerhaft fehlschlagender NTP-Versuch (kein WLAN in Reichweite) nicht bei JEDEM
// Standby den Akku mit ~14 s Funk verbrennt. Zähler/Zeitpunkt im RTC-RAM, damit
// sie den Deep Sleep überleben (auf Kaltstart/Stromausfall absichtlich zurück).
constexpr uint32_t kBackoffBaseSecs = 1800;       // 30 min nach 1. Fehlschlag
constexpr uint32_t kBackoffMaxSecs  = 24UL * 3600;// Deckel 24 h
constexpr uint8_t  kBackoffMaxShift = 6;          // 30min … 32h → gedeckelt auf 24h
RTC_DATA_ATTR uint8_t  s_failCount        = 0;
RTC_DATA_ATTR uint32_t s_lastAttemptEpoch = 0;    // Systemzeit des letzten Versuchs

uint32_t    s_lastSyncEpoch = 0;          // Epoche des letzten bestätigten Syncs
bool        s_lastSyncWasNtp = false;     // war der letzte Sync ein präzises NTP?
const char* s_source        = "—";
uint16_t    s_driftPpm      = kDefaultPpm;

// SNTP-Callback feuert, sobald die Systemzeit neu gesetzt wurde.
volatile bool s_ntpFresh = false;
uint32_t      s_preClock  = 0;            // Uhrstand beim Start des NTP-Versuchs
uint32_t      s_preMillis = 0;

enum NtpState { IDLE, WAITING };
NtpState s_ntpState     = IDLE;
uint32_t s_ntpStartMs   = 0;
bool     s_sessionSynced = false;         // schon in dieser WLAN-Sitzung gesynct?
uint32_t s_lastNvsMs    = 0;

void onNtpCb(struct timeval*) { s_ntpFresh = true; }

void beginNtpAttempt() {
  s_ntpFresh  = false;
  s_preClock  = mesh_client::clockNow();
  s_preMillis = millis();
  configTime(0, 0, NTP1, NTP2);   // UTC (kein gmt/dst-Offset) — Epoch-Konvention
}

// NTP hat die Systemzeit gesetzt: Drift gegen den Erwartungswert lernen, dann den
// Sync als bestätigt verbuchen und (gedrosselt) ins NVS sichern.
void handleNtpSynced() {
  uint32_t trueNow = (uint32_t)time(nullptr);
  // Was die Uhr ohne Korrektur jetzt anzeigen würde:
  uint32_t freeRun = s_preClock + (millis() - s_preMillis) / 1000;
  int32_t  errSecs = (int32_t)(freeRun - trueNow);

  // Drift nur zwischen zwei präzisen NTP-Syncs lernen — ein Advert-Baseline ist
  // zu ungenau (±Sekunden) und würde als überhöhte Drift durchschlagen.
  if (s_lastSyncWasNtp && s_lastSyncEpoch > kSaneEpoch && trueNow > s_lastSyncEpoch) {
    uint32_t interval = trueNow - s_lastSyncEpoch;
    if (interval >= kMinLearnSecs) {
      uint32_t obs = (uint32_t)(((uint64_t)(errSecs < 0 ? -errSecs : errSecs)
                                 * 1000000ULL) / interval);
      uint32_t ema = ((uint32_t)s_driftPpm * 3 + obs + 2) / 4;
      if (ema < kMinPpm) ema = kMinPpm;
      if (ema > kMaxPpm) ema = kMaxPpm;
      s_driftPpm = (uint16_t)ema;
      settings::setClockPpm(s_driftPpm);
      Serial.printf("[TIME] Drift gelernt: %ld s / %lu s ⇒ %u ppm\n",
                    (long)errSecs, (unsigned long)interval, (unsigned)s_driftPpm);
    }
  }

  mesh_client::setRtcTime(trueNow, true);   // setzt auch s_clockConfident im Mesh
  s_lastSyncEpoch  = trueNow;
  s_lastSyncWasNtp = true;
  s_source         = "NTP";
  s_ntpFresh       = false;
  settings::setLastTime(trueNow);
  s_lastNvsMs     = millis();
  Serial.printf("[TIME] NTP-Sync: %lu (Fehler war ~%ld s)\n",
                (unsigned long)trueNow, (long)errSecs);
}

// Eigene, blockierende WLAN→NTP-Sequenz. Nur dort aufrufen, wo das vertretbar ist
// (Pre-Standby: Gerät idle + Audio gestoppt; oder Konsole auf Nutzerwunsch).
// Schaltet WLAN danach immer wieder aus. true = Uhr frisch gesetzt.
bool blockingNtp() {
  char ssid[33], pass[65];
  settings::wifiSsid(ssid, sizeof(ssid));
  settings::wifiPass(pass, sizeof(pass));
  if (!ssid[0]) { Serial.println("[TIME] Kein WLAN konfiguriert"); return false; }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < kConnectTimeoutMs) delay(100);

  bool ok = false;
  if (WiFi.status() == WL_CONNECTED) {
    beginNtpAttempt();
    uint32_t t1 = millis();
    while (!s_ntpFresh && millis() - t1 < kNtpWaitMs) delay(100);
    if (s_ntpFresh) { handleNtpSynced(); ok = true; }
    else            Serial.println("[TIME] NTP-Timeout");
  } else {
    Serial.println("[TIME] WLAN-Connect-Timeout");
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return ok;
}

}  // namespace

namespace timesync {

void begin() {
  applyTimezone();
  sntp_set_time_sync_notification_cb(onNtpCb);
  uint16_t ppm = settings::clockPpm();
  s_driftPpm = ppm ? ppm : kDefaultPpm;

  uint32_t sys = (uint32_t)time(nullptr);
  if (sys < kSaneEpoch) {
    // Kaltstart (Stromausfall/Reset/Reflash): aus NVS wiederherstellen.
    uint32_t saved = settings::lastTime();
    if (saved > kSaneEpoch) {
      mesh_client::setRtcTime(saved, false);   // nicht autoritativ — darf korrigiert werden
      s_source = "NVS";
      Serial.printf("[TIME] Uhr aus NVS wiederhergestellt: %lu\n", (unsigned long)saved);
    } else {
      Serial.println("[TIME] Kaltstart ohne gespeicherte Zeit — warte auf Mesh/NTP");
    }
  } else {
    // Systemzeit hat (Deep Sleep) überlebt — übernehmen, aber als unbestätigt
    // führen (Drift über die Schlafdauer unbekannt ⇒ Qualität "schlecht").
    s_source = "NVS";
    Serial.printf("[TIME] Systemzeit nach Wake: %lu (unbestätigt)\n", (unsigned long)sys);
  }
}

void poll() {
  webfm::State w = webfm::state();
  if (w == webfm::State::RUNNING) {
    if (!s_sessionSynced) {
      if (s_ntpState == IDLE) {
        beginNtpAttempt();
        s_ntpState   = WAITING;
        s_ntpStartMs = millis();
      } else if (s_ntpFresh) {
        handleNtpSynced();
        s_sessionSynced = true;
        s_ntpState      = IDLE;
      } else if (millis() - s_ntpStartMs > kNtpTimeoutMs) {
        s_ntpState      = IDLE;
        s_sessionSynced = true;   // diese Sitzung aufgeben (kein Spam)
        Serial.println("[TIME] NTP-Timeout (opportunistisch)");
      }
    }
  } else {
    s_sessionSynced = false;
    s_ntpState      = IDLE;
  }

  // NVS-Fallback gedrosselt sichern.
  uint32_t cur = now();
  if (cur > kSaneEpoch && millis() - s_lastNvsMs > kNvsSaveMs) {
    s_lastNvsMs = millis();
    settings::setLastTime(cur);
  }
}

void syncBeforeStandby() {
  if (!isStale()) return;                          // Qualität reicht — nichts tun
  if (webfm::state() != webfm::State::OFF) return; // WLAN schon in Benutzung
  char ssid[33];
  settings::wifiSsid(ssid, sizeof(ssid));
  if (!ssid[0]) return;                            // keine Zugangsdaten

  // Back-off: nach Fehlschlägen nicht bei jedem Standby neu funken. now() ist
  // die ESP32-Systemzeit — auch bei (noch) falschem Absolutwert monoton, der
  // Delta-Vergleich stimmt also selbst ohne je gesetzte Uhr.
  uint32_t cur = now();
  if (s_failCount > 0 && s_lastAttemptEpoch != 0 && cur >= s_lastAttemptEpoch) {
    uint8_t  shift = s_failCount > kBackoffMaxShift ? kBackoffMaxShift : s_failCount;
    uint32_t wait  = kBackoffBaseSecs << shift;
    if (wait > kBackoffMaxSecs) wait = kBackoffMaxSecs;
    if (cur - s_lastAttemptEpoch < wait) {
      Serial.printf("[TIME] Pre-Standby-Sync übersprungen (Back-off: %u Fehlschläge, "
                    "warte %lus)\n", (unsigned)s_failCount, (unsigned long)wait);
      return;
    }
  }

  Serial.println("[TIME] Pre-Standby: Uhr veraltet — kurzer NTP-Sync ...");
  s_lastAttemptEpoch = cur;
  if (blockingNtp())            s_failCount = 0;
  else if (s_failCount < 0xFF)  s_failCount++;
}

void onExternalSync(uint32_t epoch, const char* src) {
  // Die Uhr wurde bereits vom Aufrufer (mesh_client) gesetzt — hier nur Freshness.
  if (epoch <= kSaneEpoch) return;
  s_lastSyncEpoch  = epoch;
  s_lastSyncWasNtp = false;
  s_source         = src;
}

void applyTimezone() {
  char tz[48];
  settings::tzString(tz, sizeof(tz));
  setenv("TZ", tz, 1);
  tzset();
}

void setManualTime(uint32_t utc) {
  if (utc <= kSaneEpoch) return;
  mesh_client::setRtcTime(utc, true);
  s_lastSyncEpoch  = utc;
  s_lastSyncWasNtp = false;          // manuell ist nicht für Drift-Lernen tauglich
  s_source         = "manuell";
  s_failCount      = 0;
  settings::setLastTime(utc);
  Serial.printf("[TIME] Uhr manuell gesetzt: %lu UTC\n", (unsigned long)utc);
}

bool forceSyncNow() {
  if (webfm::state() != webfm::State::OFF) {
    Serial.println("[TIME] WLAN läuft bereits (webfm) — Sync erfolgt opportunistisch");
    return false;
  }
  bool ok = blockingNtp();
  if (ok) s_failCount = 0;
  return ok;
}

uint32_t now() { return mesh_client::clockNow(); }

uint32_t estErrSeconds() {
  if (s_lastSyncEpoch <= kSaneEpoch) return 0xFFFFFFFFUL;  // nie bestätigt ⇒ schlecht
  uint32_t cur = now();
  if (cur <= s_lastSyncEpoch) return 0;
  uint64_t err = (uint64_t)s_driftPpm * (cur - s_lastSyncEpoch) / 1000000ULL;
  return err > 0xFFFFFFFFUL ? 0xFFFFFFFFUL : (uint32_t)err;
}

bool isStale() { return estErrSeconds() > kStaleSeconds; }

const char* source() { return s_source; }

const char* qualityStr() {
  uint32_t e = estErrSeconds();
  if (e < 60)           return "gut";
  if (e <= kStaleSeconds) return "mäßig";
  return "schlecht";
}

uint16_t driftPpm() { return s_driftPpm; }

}  // namespace timesync
