// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "alarmclock.h"
#include "config.h"
#include "services/audio.h"
#include "services/library.h"
#include "core/board.h"
#include "core/power.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <time.h>
#include <string.h>

namespace {

// Unter diesem Epochen-Wert (ca. 14.11.2023) gilt die Uhr als ungestellt — dann
// wird NICHT geklingelt (sonst feuert ein Kaltstart mit 1970er-Uhr sofort).
constexpr uint32_t kValidClock   = 1700000000UL;
constexpr uint32_t kSnoozeMinutes = 9;
constexpr uint32_t kRingMaxMs    = 5UL * 60UL * 1000UL;  // Auto-Quittung nach 5 min

// Generierter Wecker-Piepton auf SD (lauter, durchdringender als ein Lied). Wird
// einmalig erzeugt, wenn er fehlt; der Nutzer kann mit `alarm sound <pfad>` einen
// eigenen Klingelton setzen (dann wird dieser statt des Pieptons gespielt).
const char*        kBeepPath     = "/.fennek/alarm.wav";
constexpr uint8_t  kBeepVersion  = 2;       // Inhalts-Version (NVS „bv"); bei Änderung erhöhen → Neuerzeugung
constexpr uint32_t kBeepRate     = 22050;   // Hz, 16-bit mono
constexpr uint32_t kBeepToneHz   = 3000;    // durchdringender Pfeifton (kleine Lautsprecher effizient ~3 kHz)
constexpr int16_t  kBeepAmp      = 32000;   // Vollaussteuerung (max. mögliche Lautstärke)
constexpr uint32_t kBeepSegMs    = 200;     // 200 ms Ton / 200 ms Pause
constexpr uint32_t kBeepLenMs    = 3000;    // 3-s-Schleife (Repeat One loopt)
constexpr uint32_t kBlinkMs      = 400;     // Tastatur-Backlight-Blinktakt

alarmclock::Alarm s_alarms[alarmclock::kMaxAlarms] = {};
char        s_soundPath[TRACK_PATH_LEN] = "";
uint8_t     s_ringSignal = alarmclock::SIG_BOTH;   // Signal des aktiven Klingelns
bool        s_loaded = false;

bool     s_ringing      = false;
uint32_t s_ringStartMs  = 0;
uint32_t s_lastFireMin  = 0;   // Epoche/60 des letzten Auslösens (Entprellung)
uint8_t  s_savedVol     = AUDIO_VOL_DEFAULT;   // Lautstärke vor dem Klingeln
bool     s_blinkOn      = false;
uint32_t s_blinkMs      = 0;   // letzter Backlight-Umschaltzeitpunkt

// Schlummer-Ziel im RTC-RAM, damit ein Standby während des Schlummerns ihn
// trotzdem aufweckt (nextDueEpoch bezieht ihn ein).
RTC_DATA_ATTR uint32_t s_snoozeEpoch = 0;

Preferences s_prefs;

// byte0: bit0=enabled, bits1-3=signal (1..7, Bitmaske Ton/Blink/Vibra), bit4=einmalig.
uint32_t packAlarm(const alarmclock::Alarm& a) {
  uint8_t sig = (a.signal >= 1 && a.signal <= 7) ? a.signal : alarmclock::SIG_BOTH;
  uint8_t b0  = (uint8_t)((a.enabled ? 1 : 0) | (sig << 1) | (a.once ? 0x10 : 0));
  return (uint32_t)b0 | ((uint32_t)a.hour << 8) |
         ((uint32_t)a.minute << 16) | ((uint32_t)a.dowMask << 24);
}
alarmclock::Alarm unpackAlarm(uint32_t v) {
  alarmclock::Alarm a;
  a.enabled = (v & 0x01) != 0;
  uint8_t sig = (v >> 1) & 0x07;
  a.signal  = sig ? sig : alarmclock::SIG_BOTH;   // Altdaten (0) → beides
  a.once    = (v & 0x10) != 0;
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
  if (s_prefs.isKey("snd")) s_prefs.getString("snd", s_soundPath, sizeof(s_soundPath));
}

void persist(int i) {
  char key[4] = "a0";
  key[1] = '0' + i;
  s_prefs.putUInt(key, packAlarm(s_alarms[i]));
}

// Wochentag-Index 0=Mo .. 6=So aus struct tm (tm_wday: 0=So..6=Sa).
int monIndex(const struct tm& lt) { return (lt.tm_wday + 6) % 7; }

bool dowMatches(const alarmclock::Alarm& a, const struct tm& lt) {
  // Einmalige Wecker feuern am nächsten hh:mm, unabhängig vom Wochentag.
  return a.once || a.dowMask == 0 || (a.dowMask & (1 << monIndex(lt)));
}

// Tastatur-Backlight (GPIO42) schalten. Auf Boards ohne bestücktes Backlight
// gefahrlos (GPIO42 ist dort frei).
void kbBacklight(bool on) {
  pinMode(PIN_KB_BL, OUTPUT);
  digitalWrite(PIN_KB_BL, on ? HIGH : LOW);
}

// Erzeugt einmalig einen lauten Wecker-Piepton als 16-bit-Mono-WAV auf der SD
// (Beep/Pause-Rechteckmuster). UI-Thread; SD gechunkt unter spiLock. No-op, wenn
// die Datei schon existiert oder keine SD da ist. Eine offene File über
// spiUnlock-Lücken zu halten ist ok — in der Lücke passiert kein SD-I/O.
void ensureBeepWav() {
  if (!board::sdReady()) return;
  ensureLoaded();                                  // s_prefs offen (NVS-Versionsmarke)
  spiLock();
  bool exists = SD.exists(kBeepPath);
  spiUnlock();
  if (exists && s_prefs.getUChar("bv", 0) == kBeepVersion) return;  // aktuell → fertig

  const uint32_t nSamples   = kBeepRate * kBeepLenMs / 1000;
  const uint32_t dataBytes  = nSamples * 2;
  const uint32_t segSamples = kBeepRate * kBeepSegMs / 1000;
  const uint32_t halfPeriod = kBeepRate / (2 * kBeepToneHz);

  spiLock();
  File f = SD.open(kBeepPath, FILE_WRITE);
  if (!f) { spiUnlock(); Serial.println("[ALARM] Piepton: SD-Open fehlgeschlagen"); return; }
  uint8_t hdr[44];
  auto p32 = [](uint8_t* d, uint32_t v){ d[0]=v; d[1]=v>>8; d[2]=v>>16; d[3]=v>>24; };
  auto p16 = [](uint8_t* d, uint16_t v){ d[0]=v; d[1]=v>>8; };
  memcpy(hdr, "RIFF", 4);       p32(hdr+4, 36 + dataBytes);  memcpy(hdr+8,  "WAVE", 4);
  memcpy(hdr+12, "fmt ", 4);    p32(hdr+16, 16);
  p16(hdr+20, 1);               p16(hdr+22, 1);               p32(hdr+24, kBeepRate);
  p32(hdr+28, kBeepRate * 2);   p16(hdr+32, 2);               p16(hdr+34, 16);
  memcpy(hdr+36, "data", 4);    p32(hdr+40, dataBytes);
  f.write(hdr, 44);
  spiUnlock();

  int16_t buf[512];
  uint32_t i = 0;
  while (i < nSamples) {
    int n = 0;
    while (n < 512 && i < nSamples) {
      bool beep = ((i / segSamples) & 1) == 0;                      // gerade Segmente = Ton
      buf[n++] = beep ? (((i / halfPeriod) & 1) ? kBeepAmp : (int16_t)-kBeepAmp) : 0;
      i++;
    }
    spiLock();
    f.write((const uint8_t*)buf, n * 2);
    spiUnlock();
  }
  spiLock(); f.close(); spiUnlock();
  s_prefs.putUChar("bv", kBeepVersion);            // Inhalts-Version vermerken
  Serial.printf("[ALARM] Piepton erzeugt (v%u): %s (%lu Samples)\n",
                (unsigned)kBeepVersion, kBeepPath, (unsigned long)nSamples);
}

// Klingeln starten: voll aufdrehen, Piepton (oder eigener Klingelton) in
// Dauerschleife, Tastatur-Backlight als Sichtsignal. Lautstärke wird gemerkt
// und beim Quittieren/Schlummern wiederhergestellt (kein NVS-Schreiben).
void startRing(const char* why, uint8_t signal) {
  s_ringSignal = (signal >= 1 && signal <= 7) ? signal : alarmclock::SIG_BOTH;
  const bool wantTone  = (s_ringSignal & alarmclock::SIG_TONE)  != 0;
  const bool wantBlink = (s_ringSignal & alarmclock::SIG_BLINK) != 0;
  char path[TRACK_PATH_LEN] = "";
  if (wantTone) {
    strncpy(path, s_soundPath, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    if (!path[0]) {                                // kein eigener Ton → Piepton
      ensureBeepWav();
      if (board::sdReady()) { strncpy(path, kBeepPath, sizeof(path) - 1); path[sizeof(path) - 1] = '\0'; }
    }
    if (!path[0] && library::count() > 0) library::path(0, path, sizeof(path));  // Notfall
    if (path[0]) {
      s_savedVol = audio::status().volume;         // Lautstärke merken ...
      audio::setVolume(AUDIO_VOL_MAX);             // ... und voll aufdrehen
      audio::queueBegin(audio::Owner::Music);
      audio::queueAdd(path);
      audio::queueCommit(0);
      audio::setRepeat(audio::Repeat::One);
    } else {
      Serial.println("[ALARM] Kein Klingelton + keine SD — nur Backlight");
    }
  }
  const bool wantVibra = (s_ringSignal & alarmclock::SIG_VIBRA) != 0;
  s_ringing = true;
  s_ringStartMs = millis();
  s_blinkOn = true; s_blinkMs = millis();
  if (wantBlink) kbBacklight(true);
  if (wantVibra) { board::vibraEnable(true); board::vibrate(true); }
  power::noteActivity();   // Auto-Standby nicht mitten ins Klingeln grätschen
  Serial.printf("[ALARM] Klingelt (%s) [%s%s%s]: %s\n", why,
                wantTone ? "Ton " : "", wantBlink ? "Blink " : "",
                wantVibra ? "Vibra" : "", path[0] ? path : "-");
}

}  // namespace

namespace alarmclock {

void begin() { ensureLoaded(); ensureBeepWav(); }   // Piepton einmalig vorab erzeugen

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

uint32_t snoozeMinutes() { return kSnoozeMinutes; }

void fireNow() {   // Test löst alle drei Signale aus (Ton + Blinken + Vibration)
  startRing("Test", alarmclock::SIG_TONE | alarmclock::SIG_BLINK | alarmclock::SIG_VIBRA);
}

// Klingel-Ausgabe stoppen: Audio aus, Lautstärke zurück, Backlight + Motor aus.
void stopRingOutput() {
  audio::stop();
  audio::setVolume(s_savedVol);
  kbBacklight(false);
  board::vibraEnable(false);   // Motor aus + Treiber zurück in Shutdown
}

void snooze() {
  if (!s_ringing) return;
  stopRingOutput();
  s_ringing = false;
  uint32_t now = (uint32_t)time(nullptr);
  s_snoozeEpoch = now + kSnoozeMinutes * 60;
  Serial.printf("[ALARM] Schlummer %lu min\n", (unsigned long)kSnoozeMinutes);
}

void dismiss() {
  if (s_ringing) stopRingOutput();
  s_ringing = false;
  s_snoozeEpoch = 0;
  Serial.println("[ALARM] Quittiert");
}

void poll() {
  ensureLoaded();

  if (s_ringing) {
    power::noteActivity();                       // wach bleiben, solange es klingelt
    uint32_t ms = millis();
    if (ms - s_blinkMs >= kBlinkMs) {            // Backlight + Motor im Takt pulsen
      s_blinkMs = ms; s_blinkOn = !s_blinkOn;
      if (s_ringSignal & alarmclock::SIG_BLINK) kbBacklight(s_blinkOn);
      if (s_ringSignal & alarmclock::SIG_VIBRA) board::vibrate(s_blinkOn);
    }
    if (ms - s_ringStartMs >= kRingMaxMs) {
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
    startRing("Schlummer", s_ringSignal);
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
      startRing(w, s_alarms[i].signal);
      if (s_alarms[i].once) { s_alarms[i].enabled = false; persist(i); }  // einmalig: danach aus
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
