// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "alarmclock.h"
#include "config.h"
#include "services/audio.h"
#include "services/library.h"
#include "core/power.h"

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <string.h>

namespace {

// Unter diesem Epochen-Wert (ca. 14.11.2023) gilt die Uhr als ungestellt — dann
// wird NICHT geklingelt (sonst feuert ein Kaltstart mit 1970er-Uhr sofort).
constexpr uint32_t kValidClock   = 1700000000UL;
constexpr uint32_t kSnoozeMinutes = 9;
constexpr uint32_t kRingMaxMs    = 5UL * 60UL * 1000UL;  // Auto-Quittung nach 5 min

alarmclock::Alarm s_alarms[alarmclock::kMaxAlarms] = {};
char        s_soundPath[TRACK_PATH_LEN] = "";
bool        s_loaded = false;

bool     s_ringing      = false;
uint32_t s_ringStartMs  = 0;
uint32_t s_lastFireMin  = 0;   // Epoche/60 des letzten Auslösens (Entprellung)

// Schlummer-Ziel im RTC-RAM, damit ein Standby während des Schlummerns ihn
// trotzdem aufweckt (nextDueEpoch bezieht ihn ein).
RTC_DATA_ATTR uint32_t s_snoozeEpoch = 0;

Preferences s_prefs;

uint32_t packAlarm(const alarmclock::Alarm& a) {
  return (uint32_t)(a.enabled ? 1 : 0) | ((uint32_t)a.hour << 8) |
         ((uint32_t)a.minute << 16) | ((uint32_t)a.dowMask << 24);
}
alarmclock::Alarm unpackAlarm(uint32_t v) {
  alarmclock::Alarm a;
  a.enabled = (v & 0xFF) != 0;
  a.hour    = (v >> 8) & 0xFF;
  a.minute  = (v >> 16) & 0xFF;
  a.dowMask = (v >> 24) & 0xFF;
  return a;
}

void ensureLoaded() {
  if (s_loaded) return;
  s_loaded = true;
  if (!s_prefs.begin("alarm", false)) return;
  char key[4] = "a0";
  for (int i = 0; i < alarmclock::kMaxAlarms; i++) {
    key[1] = '0' + i;
    s_alarms[i] = unpackAlarm(s_prefs.getUInt(key, 0));
  }
  s_prefs.getString("snd", s_soundPath, sizeof(s_soundPath));
}

void persist(int i) {
  char key[4] = "a0";
  key[1] = '0' + i;
  s_prefs.putUInt(key, packAlarm(s_alarms[i]));
}

// Wochentag-Index 0=Mo .. 6=So aus struct tm (tm_wday: 0=So..6=Sa).
int monIndex(const struct tm& lt) { return (lt.tm_wday + 6) % 7; }

bool dowMatches(const alarmclock::Alarm& a, const struct tm& lt) {
  return a.dowMask == 0 || (a.dowMask & (1 << monIndex(lt)));
}

// Klingeln starten: Klingelton (oder erster Bibliothekstitel) in Dauerschleife.
void startRing(const char* why) {
  char path[TRACK_PATH_LEN];
  strncpy(path, s_soundPath, sizeof(path) - 1);
  path[sizeof(path) - 1] = '\0';
  if (!path[0]) {
    if (library::count() > 0) library::path(0, path, sizeof(path));
  }
  if (!path[0]) {
    Serial.println("[ALARM] Kein Klingelton + leere Bibliothek — klingelt nicht");
    return;
  }
  audio::queueBegin(audio::Owner::Music);
  audio::queueAdd(path);
  audio::queueCommit(0);
  audio::setRepeat(audio::Repeat::One);
  s_ringing = true;
  s_ringStartMs = millis();
  power::noteActivity();   // Auto-Standby nicht mitten ins Klingeln grätschen
  Serial.printf("[ALARM] Klingelt (%s): %s\n", why, path);
}

}  // namespace

namespace alarmclock {

void begin() { ensureLoaded(); }

int   count() { return kMaxAlarms; }
Alarm get(int i) { ensureLoaded(); return (i >= 0 && i < kMaxAlarms) ? s_alarms[i] : Alarm{}; }

void set(int i, const Alarm& a) {
  ensureLoaded();
  if (i < 0 || i >= kMaxAlarms) return;
  s_alarms[i] = a;
  persist(i);
}

void clear(int i) {
  ensureLoaded();
  if (i < 0 || i >= kMaxAlarms) return;
  s_alarms[i].enabled = false;
  persist(i);
}

uint32_t nextDueEpoch(uint32_t from) {
  ensureLoaded();
  uint32_t best = 0;
  // Schlummer ist ein einmaliger, exakter Zeitpunkt.
  if (s_snoozeEpoch && s_snoozeEpoch >= from) best = s_snoozeEpoch;

  for (int i = 0; i < kMaxAlarms; i++) {
    if (!s_alarms[i].enabled) continue;
    // Bis zu 7 Tage vorausrechnen, frühesten Treffer dieses Weckers nehmen.
    for (int d = 0; d < 8; d++) {
      time_t ft = (time_t)from;
      struct tm c;
      localtime_r(&ft, &c);
      c.tm_mday += d;
      c.tm_hour = s_alarms[i].hour;
      c.tm_min  = s_alarms[i].minute;
      c.tm_sec  = 0;
      c.tm_isdst = -1;               // mktime soll DST selbst bestimmen
      time_t e = mktime(&c);         // normalisiert + setzt tm_wday
      if (e == (time_t)-1) break;
      if ((uint32_t)e < from) continue;
      if (!dowMatches(s_alarms[i], c)) continue;
      if (best == 0 || (uint32_t)e < best) best = (uint32_t)e;
      break;
    }
  }
  return best;
}

bool ringing() { return s_ringing; }

void fireNow() { startRing("Test"); }

void snooze() {
  if (!s_ringing) return;
  audio::stop();
  s_ringing = false;
  uint32_t now = (uint32_t)time(nullptr);
  s_snoozeEpoch = now + kSnoozeMinutes * 60;
  Serial.printf("[ALARM] Schlummer %lu min\n", (unsigned long)kSnoozeMinutes);
}

void dismiss() {
  if (s_ringing) audio::stop();
  s_ringing = false;
  s_snoozeEpoch = 0;
  Serial.println("[ALARM] Quittiert");
}

void poll() {
  ensureLoaded();

  if (s_ringing) {
    power::noteActivity();                       // wach bleiben, solange es klingelt
    if (millis() - s_ringStartMs >= kRingMaxMs) {
      Serial.println("[ALARM] Auto-Quittung nach 5 min");
      dismiss();
    }
    return;
  }

  uint32_t now = (uint32_t)time(nullptr);
  if (now < kValidClock) return;                 // Uhr ungestellt → nicht klingeln

  // Schlummer-Ziel erreicht?
  if (s_snoozeEpoch && now >= s_snoozeEpoch && now - s_snoozeEpoch < 90) {
    s_snoozeEpoch = 0;
    startRing("Schlummer");
    return;
  }
  if (s_snoozeEpoch && now >= s_snoozeEpoch) s_snoozeEpoch = 0;  // verpasst → verfallen

  // Minutengenaue Auslösung; pro Minute höchstens einmal (Entprellung).
  uint32_t curMin = now / 60;
  if (curMin == s_lastFireMin) return;

  time_t nt = (time_t)now;
  struct tm lt;
  localtime_r(&nt, &lt);
  for (int i = 0; i < kMaxAlarms; i++) {
    if (!s_alarms[i].enabled) continue;
    if (lt.tm_hour == s_alarms[i].hour && lt.tm_min == s_alarms[i].minute &&
        dowMatches(s_alarms[i], lt)) {
      s_lastFireMin = curMin;
      char w[16];
      snprintf(w, sizeof(w), "Wecker %d", i);
      startRing(w);
      return;
    }
  }
}

void soundPath(char* out, size_t n) {
  ensureLoaded();
  if (!out || n == 0) return;
  strncpy(out, s_soundPath, n - 1);
  out[n - 1] = '\0';
}

void setSoundPath(const char* p) {
  ensureLoaded();
  if (!p) return;
  strncpy(s_soundPath, p, sizeof(s_soundPath) - 1);
  s_soundPath[sizeof(s_soundPath) - 1] = '\0';
  s_prefs.putString("snd", s_soundPath);
}

}  // namespace alarmclock
