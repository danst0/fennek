// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "power.h"
#include "config.h"
#include "core/battery.h"
#include "core/board.h"
#include "core/display.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "core/settings.h"
#include "core/sleep_img.h"
#include "services/alarmclock.h"
#include "services/audio.h"
#include "services/battlog.h"
#include "services/calendar.h"
#include "services/calibre_books.h"
#include "services/notes_ai.h"
#include "services/podcast.h"
#include "services/reinschrift.h"
#include "services/scrobble.h"
#include "services/settingsfile.h"
#include "services/timesync.h"
#include "services/webfm.h"

#include <Arduino.h>
#include <WiFi.h>        // WiFi-Events für den CPU-Takt-Governor
#include <Wire.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

// PIN_USER_BTN (GPIO0) ist der seitliche Ausschaltknopf — zugleich der
// Boot-Strapping-Pin. Standardverdrahtung: extern hochgezogen, gedrückt = LOW
// (aktiv-low). Aufwecken daher per ANY_LOW. Falls die Hardware-Verifikation
// eine andere Verdrahtung zeigt (idle LOW / Druck HIGH), nur diese beiden
// Konstanten umdrehen (BTN_PRESSED_LEVEL = HIGH, WAKE_LEVEL = ANY_HIGH).
#ifndef PIN_USER_BTN
#define PIN_USER_BTN 0
#endif

namespace {

constexpr int      BTN_PRESSED_LEVEL = LOW;
constexpr auto     WAKE_LEVEL        = ESP_EXT1_WAKEUP_ANY_LOW;
constexpr uint32_t LONGPRESS_MS      = 2000;   // Halten für manuellen Standby
constexpr uint32_t SHORTPRESS_MIN_MS = 40;     // Entprellung des Kurzklicks

// Stunden-Takt der Timer-Wakes; alle kTimerWakeFullEvery Wakes ein Vollbild-
// Refresh gegen E-Ink-Ghosting. SLEEP_WAKE_TEST verkürzt auf 60 s (Gerätetest).
#ifdef SLEEP_WAKE_TEST
constexpr uint64_t kTimerWakeUs = 60ULL * 1000000ULL;
#else
constexpr uint64_t kTimerWakeUs = 3600ULL * 1000000ULL;
#endif
constexpr uint32_t kTimerWakeFullEvery = 12;
constexpr int      kBannerY = 278;

uint32_t s_lastActivity = 0;
uint32_t s_btnDownAt    = 0;     // 0 = Knopf nicht gedrückt
bool     s_armed        = false; // Langdruck dieses Drucks schon ausgelöst?
bool     s_locked       = false; // Tastensperre aktiv?

// Batterie-% fürs Schlafbild (DrawFn ist ein nackter Funktionszeiger, daher
// keine Captures möglich). Der Wake-Zähler liegt im RTC-RAM: überlebt den
// Deep Sleep ohne NVS-Verschleiß, ist nach Kaltstart/Knopf-Boot egal.
uint8_t s_sleepPct = 0;
RTC_DATA_ATTR uint32_t s_timerWakes = 0;

// --- Schlaf-Diagnose (RTC-RAM) ----------------------------------------------
// Hintergrund: drei Tiefentladungen bis 0-2 % (25.06.-07.07.26) ohne jede
// battlog-Spur — der Täter kann nur ein Pfad sein, der nichts loggt (Timer-
// Wake-Minimalpfad, enterStandby nach dem Flush, Crash-Schleife vor
// BATTLOG_BEGIN). Deshalb: Phase-Breadcrumb + Wake-Historie im RTC-RAM
// (übersteht Deep Sleep UND Panic/WDT-Resets), beim nächsten Vollboot via
// logSleepDebug() ins battlog gekippt.
enum SleepPhase : uint8_t {
  PH_SLEEP_OK = 0,    // sauber in Deep Sleep gegangen (auch Kaltstart-Default)
  PH_STBY_EXPORT,     // enterStandby: Settings-Export + battlog-Flush
  PH_STBY_RENDER,     // enterStandby: Schlafbild rendern
  PH_STBY_WAIT,       // enterStandby: Knopf-Loslass-Warteschleife
  PH_STBY_ARM,        // enterStandby: Holds/Wake-Quellen (auch Historien-Marker)
  PH_WAKE_START,      // Timer-Wake: Minimalpfad betreten (auch Historien-Marker)
  PH_WAKE_DISPLAY,    // Timer-Wake: Banner-/Vollbild-Refresh
  PH_WAKE_ARM,        // Timer-Wake: Holds/Wake-Quellen vor dem Wiedereinschlafen
};
RTC_DATA_ATTR uint8_t s_sleepPhase = PH_SLEEP_OK;

struct SleepSample {
  uint32_t epoch;   // Systemzeit des Eintrags (übersteht Deep Sleep)
  uint16_t mv;
  uint16_t rm;      // RemainingCapacity (mAh, Coulomb-Counter) — s. mA-Rechnung
  uint8_t  pct;
  uint8_t  phase;   // PH_WAKE_START = Timer-Wake, PH_STBY_ARM = Standby-Beginn
};
constexpr int kSleepHistMax = 48;
RTC_DATA_ATTR SleepSample s_sleepHist[kSleepHistMax];
RTC_DATA_ATTR uint8_t s_sleepHistN = 0;   // belegte Einträge (gedeckelt)
RTC_DATA_ATTR uint8_t s_sleepHistW = 0;   // Schreibindex (Ring)

// Crash-Schleifen-Zähler: abnormale Resets (Panic/WDT/Brownout) in Folge, ohne
// dazwischen 2 min stabile Laufzeit (poll() löscht) oder sauberen Standby.
RTC_DATA_ATTR uint8_t s_abnormalBoots = 0;

void histAdd(uint8_t phase, uint8_t pct, uint16_t mv, uint16_t rm) {
  SleepSample& e = s_sleepHist[s_sleepHistW];
  e.epoch = (uint32_t)time(nullptr);
  e.mv    = mv;
  e.rm    = rm;
  e.pct   = pct;
  e.phase = phase;
  s_sleepHistW = (uint8_t)((s_sleepHistW + 1) % kSleepHistMax);
  if (s_sleepHistN < kSleepHistMax) s_sleepHistN++;
}

bool abnormalReset(esp_reset_reason_t rr) {
  return rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
         rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT;
}

// --- Tiefentladeschutz --------------------------------------------------------
// Unter kCritPct (auf Batterie) keine Stunden-Wakes mehr — jeder Wake drückt
// die fast leere Zelle weiter Richtung BMS-Abschaltung (2,9 V, battery.log
// 05.-07.07.26). Der Wecker-Wake bleibt bis kCritAlarmPct erhalten.
constexpr uint8_t kCritPct      = 5;
constexpr uint8_t kCritAlarmPct = 3;

// Absoluter Weckzeitpunkt (UTC-Epoche) des nächsten Weckers, beim Einschlafen
// mit gültiger Zeitzone berechnet. 0 = kein Wecker. Liegt im RTC-RAM (übersteht
// Deep Sleep), damit auch ein Stunden-Wake daraus die Restzeit ableiten kann.
RTC_DATA_ATTR uint32_t s_alarmWakeEpoch = 0;
// Toleranz, ab der ein Timer-Wake als Wecker-Wake gilt (Vollboot). Klein, da der
// Wake-Timer ohnehin auf den Weckzeitpunkt gestellt wird.
constexpr uint32_t kAlarmWakeSlackSec = 30;

bool btnPressed() { return digitalRead(PIN_USER_BTN) == BTN_PRESSED_LEVEL; }

// Timer-Wake-Dauer: Stunden-Takt, aber früher, wenn ein Wecker näher liegt.
// Reine Epochen-Arithmetik (s_alarmWakeEpoch ist absolut) — funktioniert auch
// im Minimal-Wake-Pfad ohne gesetzte Zeitzone. Untergrenze 1 s.
uint64_t nextWakeUs() {
  uint64_t dur = kTimerWakeUs;
  if (s_alarmWakeEpoch) {
    uint32_t now = (uint32_t)time(nullptr);
    uint64_t untilUs = (now < s_alarmWakeEpoch)
        ? (uint64_t)(s_alarmWakeEpoch - now) * 1000000ULL
        : 1000000ULL;               // fällig/überfällig → gleich wieder wach
    if (untilUs < dur) dur = untilUs;
  }
  if (dur < 1000000ULL) dur = 1000000ULL;
  return dur;
}

// Banner unten: schwarzes Band mit weißem Text (Bild ist dort dunkel) —
// Titel, Aufweck-Hinweis und klein die Batterie-% (stündlich erneuert).
// Separat, damit der Timer-Wake nur diesen Streifen partiell refresht.
void drawSleepBanner(Adafruit_GFX& g) {
  g.fillRect(0, kBannerY, kSleepImgW, kSleepImgH - kBannerY, GxEPD_BLACK);
  g.drawFastHLine(0, kBannerY, kSleepImgW, GxEPD_WHITE);
  g.setTextColor(GxEPD_WHITE);
  gui::printAt(g, 120 - 53, kBannerY + 6, "Fennek", 3);
  // Aufweck-Hinweis dynamisch zentrieren (Textbreite ist sprachabhängig).
  const char* hint = i18n::tr(i18n::Str::SleepWakeHint);
  g.setTextSize(1);
  uint16_t hw, hh;
  gui::textBounds(g, hint, &hw, &hh);
  gui::printAt(g, (kSleepImgW - (int)hw) / 2, kBannerY + 31, hint, 1);
  // Batterie-% rechtsbündig in der Hinweis-Zeile.
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", (unsigned)s_sleepPct);
  uint16_t pw, ph;
  gui::textBounds(g, pct, &pw, &ph);
  gui::printAt(g, kSleepImgW - (int)pw - 4, kBannerY + 31, pct, 1);
  g.setTextColor(GxEPD_BLACK);
}

// Standby-Bild: ein schlafender Fennek (KI-generiert, siehe sleep_img.h) als
// vollflächige 1-Bit-Bitmap, darunter das Banner. Der "by Dr. Daniel Dumke"-
// Credit ist direkt ins Bild (sleep_img.h) gebacken: kalligrafisch
// (URW Chancery) und am Fuchsschwanz entlang geschwungen (tools/sleepcredit.py).
void drawSleepScreen(Adafruit_GFX& g) {
  g.fillScreen(GxEPD_WHITE);
  g.drawBitmap(0, 0, kSleepImg, kSleepImgW, kSleepImgH, GxEPD_BLACK);
  drawSleepBanner(g);
}

// Beide Aufwach-Quellen scharf machen: Knopf (EXT1) + Stunden-Timer.
// Tiefentladeschutz: bei kritischem Akku auf Batterie kein Stunden-Timer mehr
// (nur Knopf; ein anstehender Wecker weckt noch bis kCritAlarmPct).
void armWakeups() {
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_USER_BTN, WAKE_LEVEL);
  if (s_sleepPct <= kCritPct && !battery::external()) {
    if (s_alarmWakeEpoch && s_sleepPct > kCritAlarmPct) {
      uint32_t now = (uint32_t)time(nullptr);
      uint64_t us = (now < s_alarmWakeEpoch)
          ? (uint64_t)(s_alarmWakeEpoch - now) * 1000000ULL : 1000000ULL;
      esp_sleep_enable_timer_wakeup(us);
    }
    Serial.printf("[PWR] Akku kritisch (%u%%) — Schlaf ohne Stunden-Wake\n",
                  (unsigned)s_sleepPct);
    return;
  }
  esp_sleep_enable_timer_wakeup(nextWakeUs());
}

// Alle Abschalt-/Deselect-Pegel setzen und über den Deep Sleep einfrieren.
// Ohne das Hold floaten GPIO10/41/46 hochohmig, die Rails (Peripherie/DAC/LoRa)
// kommen zurück -> ~50 mA Standby-Drain (gemessen: 94 %→5 % in ~29 h,
// battery.log 14./15.06.2026). GPIO42 (Tastatur-Backlight-Gate) ebenfalls fest
// LOW — floatete bisher im Schlaf.
//
// LORA_CS (GPIO3) wird bewusst NICHT auf HIGH getrieben, sondern als Input
// freigegeben (holdSleepPins latcht ihn dann hochohmig): Ein aktiv getriebenes
// HIGH speist den unversorgten SX1262 (LORA_EN LOW) über seine Schutzdioden
// rückwärts und verbrennt ~18 mA. Gemessen per Coulomb-Counter (Bisektion
// 16./17.07.26, n=3): CS HIGH = 22,6 mA, CS floatend = 4,0 mA Standby. Im Schlaf
// ist die CPU aus, es gibt keinen SPI-Verkehr → kein Bus-Konflikt durch das
// nicht-deselektierte CS, und das Modul ist ohnehin stromlos.
void freezeSleepPins() {
  digitalWrite(PIN_EINK_CS, HIGH);
  pinMode(PIN_LORA_CS, INPUT);   // NICHT HIGH treiben — s.o. (Back-Powering)
  digitalWrite(PIN_SD_CS,   HIGH);
  pinMode(PIN_KB_BL, OUTPUT);
  digitalWrite(PIN_KB_BL, LOW);
  board::holdSleepPins();
}

// --- CPU-Takt-Governor (siehe power.h) ------------------------------------------
// Mutex statt Spinlock: setCpuFrequencyMhz() ruft bei registrierten
// APB-Callbacks (UART-HAL) in Treiber-Locks hinein — das darf nicht in einer
// Critical Section passieren. Alle Aufrufer sind Tasks (Audio Core 0, Schach,
// Loop, WiFi-Event-Task), nie ISRs.
SemaphoreHandle_t s_boostMux = nullptr;
int  s_boostN      = 0;      // aktive 240-MHz-Anforderungen
bool s_boostGovern = false;  // erst ab boostBegin(); der Boot läuft mit 240 MHz

void applyBoostLocked() {    // nur unter s_boostMux aufrufen
  if (!s_boostGovern) return;
  setCpuFrequencyMhz(s_boostN > 0 ? 240 : 80);   // no-op bei unverändertem Takt
}

// Zentraler WLAN-Haken: jede WiFi-Nutzung (webfm, Scrobble, OTA, Kalender,
// Calibre, Todo, NTP, …) boostet automatisch — schnellerer Transfer heißt
// kürzere Funkzeit, das WLAN-Radio (~100 mA) dominiert die Rechnung. Läuft im
// WiFi-Event-Task und erreicht so auch blockierende Sync-Flows.
void onWifiBoost(arduino_event_id_t ev) {
  static bool s_wifiHeld = false;   // nur vom Event-Task berührt
  bool up = (ev == ARDUINO_EVENT_WIFI_STA_START || ev == ARDUINO_EVENT_WIFI_AP_START);
  if (up && !s_wifiHeld) {
    s_wifiHeld = true;
    power::boostLock();
  } else if (!up && s_wifiHeld && WiFi.getMode() == WIFI_OFF) {
    // STA_STOP/AP_STOP: erst freigeben, wenn wirklich alles aus ist
    // (webfm kann zwischen STA und AP wechseln).
    s_wifiHeld = false;
    power::boostUnlock();
  }
}

}  // namespace

namespace power {

void begin() {
  pinMode(PIN_USER_BTN, INPUT);   // externer Pull-up am Strapping-Pin
  s_lastActivity = millis();
  s_boostMux = xSemaphoreCreateMutex();
}

void boostLock() {
  if (!s_boostMux) return;   // vor begin(): Boot läuft ohnehin mit 240 MHz
  xSemaphoreTake(s_boostMux, portMAX_DELAY);
  s_boostN++;
  applyBoostLocked();
  xSemaphoreGive(s_boostMux);
}

void boostUnlock() {
  if (!s_boostMux) return;
  xSemaphoreTake(s_boostMux, portMAX_DELAY);
  if (s_boostN > 0) s_boostN--;
  applyBoostLocked();
  xSemaphoreGive(s_boostMux);
}

void boostBegin() {
  WiFi.onEvent(onWifiBoost, ARDUINO_EVENT_WIFI_STA_START);
  WiFi.onEvent(onWifiBoost, ARDUINO_EVENT_WIFI_STA_STOP);
  WiFi.onEvent(onWifiBoost, ARDUINO_EVENT_WIFI_AP_START);
  WiFi.onEvent(onWifiBoost, ARDUINO_EVENT_WIFI_AP_STOP);
  if (!s_boostMux) return;
  xSemaphoreTake(s_boostMux, portMAX_DELAY);
  s_boostGovern = true;
  applyBoostLocked();
  xSemaphoreGive(s_boostMux);
  Serial.printf("[PWR] CPU-Governor aktiv: %lu MHz (Basis 80, Boost 240)\n",
                (unsigned long)getCpuFrequencyMhz());
}

void noteBoot() {
  // Crash-Schleifen-Bremse: drei abnormale Resets (Panic/WDT/Brownout) in
  // Folge — ohne 2 min stabile Laufzeit oder sauberen Standby dazwischen —
  // heißt Boot-Schleife (z. B. korrupte SD-Caches). Die brennt bei ~100 mA
  // den Akku in unter einem Tag leer und hinterlässt keine battlog-Spur
  // (der PSRAM-Ring stirbt mit jedem Reset). Dann: Not-Standby, nur der
  // Knopf weckt — der nächste Knopf-Wake ist ein normaler Boot-Versuch
  // (Reset-Grund DEEPSLEEP setzt den Zähler zurück).
  esp_reset_reason_t rr = esp_reset_reason();
  if (abnormalReset(rr)) s_abnormalBoots++;
  else                   s_abnormalBoots = 0;
  if (s_abnormalBoots < 3) return;

  Serial.printf("[PWR] %u abnormale Resets in Folge (zuletzt %d) — "
                "Not-Standby, nur Knopf weckt\n",
                (unsigned)s_abnormalBoots, (int)rr);
  board::powerOn();            // Pins definiert treiben, damit die Holds greifen
  board::perfPower(false);
  freezeSleepPins();
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_USER_BTN, WAKE_LEVEL);   // KEIN Timer
  s_sleepPhase = PH_SLEEP_OK;
  esp_deep_sleep_start();
}

void logSleepDebug() {
  // Nach jedem Vollboot: Wie endete die letzte Schlafperiode? PH_SLEEP_OK =
  // sauber; alles andere = Abbruch mitten in enterStandby()/Timer-Wake (etwa
  // durch den Task-Watchdog) — genau die bisher unsichtbaren Fälle.
  if (s_sleepPhase != PH_SLEEP_OK)
    Serial.printf("[PWR] Letzte Schlafphase UNSAUBER beendet: Phase=%u\n",
                  (unsigned)s_sleepPhase);
#ifdef BATTLOG
  BATTLOG_EVENT("Schlaf", "Phase=%u Wakes=%lu Crashes=%u",
                (unsigned)s_sleepPhase, (unsigned long)s_timerWakes,
                (unsigned)s_abnormalBoots);
  // Wake-Historie (RTC-RAM) als battlog-Zeilen ausgeben: eine pro Timer-Wake/
  // Standby-Beginn seit dem letzten Vollboot — zeigt beim nächsten Vorfall,
  // in welcher Stunde der Akku wie schnell fiel.
  //
  // Der mA-Wert am Zeilenende ist die eigentliche Messung: aus dem Coulomb-
  // Counter des Gauge (RemainingCapacity), nicht aus der Spannungskurve. Er gilt
  // für die Spanne seit dem VORIGEN Eintrag. Lesart:
  //   Ph=4 -> Ph=5  (Standby-Beginn bis 1. Wake) = fast reiner Deep-Sleep-Strom
  //   Ph=5 -> Ph=5  (Wake zu Wake)               = Schlaf + ein Banner-Wake
  // Erwartung für einen gesunden Deep Sleep: << 1 mA. Alles im zweistelligen
  // mA-Bereich heißt, dass eine Rail durchläuft.
  int start = (s_sleepHistW + kSleepHistMax - s_sleepHistN) % kSleepHistMax;
  for (int i = 0; i < s_sleepHistN; i++) {
    const SleepSample& e = s_sleepHist[(start + i) % kSleepHistMax];
    char ts[20] = "?";
    if (e.epoch > 1500000000UL) {
      time_t tt = (time_t)e.epoch;
      struct tm tm;
      localtime_r(&tt, &tm);
      strftime(ts, sizeof(ts), "%d.%m. %H:%M", &tm);
    }
    // Mittleren Strom seit dem vorigen Eintrag aus der Ladungsdifferenz bilden.
    // Nur wenn beide Coulomb-Werte plausibel sind (0 = I2C-Fehler) und die Uhr
    // zwischen den Einträgen vorwärts lief; sonst bleibt die Spalte leer.
    // Der mA-Wert gehört zum Intervall, das beim VORIGEN Eintrag begann — also
    // auch dessen Rail-Konfiguration (cfg), nicht die von diesem Eintrag.
    char mA[32] = "";
    if (i > 0) {
      const SleepSample& p = s_sleepHist[(start + i - 1) % kSleepHistMax];
      if (p.rm && e.rm && e.epoch > p.epoch) {
        int32_t  dmah = (int32_t)p.rm - (int32_t)e.rm;   // >0 = entladen
        uint32_t dsec = e.epoch - p.epoch;
        long     ma   = (long)(dmah * 3600 / (int32_t)dsec);
        snprintf(mA, sizeof(mA), " %ldmA", ma);
      }
    }
    BATTLOG_EVENT("Schlaf", "%s Ph=%u %u%% %umV %umAh%s",
                  ts, (unsigned)e.phase, (unsigned)e.pct, (unsigned)e.mv,
                  (unsigned)e.rm, mA);
  }
#endif
  s_sleepHistN = 0;
  s_sleepHistW = 0;
  s_sleepPhase = PH_SLEEP_OK;
}

void noteActivity() { s_lastActivity = millis(); }

uint32_t idleMs() { return millis() - s_lastActivity; }

bool locked() { return s_locked; }

void enterStandby() {
  Serial.println("[FENNEK] Standby — gehe in Deep Sleep ...");

  // Fallnetz: hängt irgendwas ab hier (SD-Export, E-Ink, I2C), beißt nach 60 s
  // der Task-Watchdog (Panic-Reset → Vollboot → battlog-Zeile → Auto-Standby)
  // statt still bei vollem Verbrauch zu stehen, bis der Akku leer ist.
  esp_task_wdt_init(60, true);
  esp_task_wdt_add(NULL);
  s_abnormalBoots = 0;   // sauberer Standby = System gesund
  s_sleepPhase = PH_STBY_EXPORT;

  // Aktuelle Uhrzeit ins NVS sichern (Fallback, falls der Akku im Schlaf stirbt;
  // die ESP32-Systemzeit selbst überlebt den Deep Sleep ohne Zutun).
  uint32_t t = timesync::now();
  if (t > 1781000000UL) settings::setLastTime(t);

  // 1) Audio + DAC + Radio stilllegen.
  audio::stop();
  delay(60);                 // Audio-Task das Kommando abarbeiten lassen
  board::dacPower(false);
  board::loraPower(false);
  board::gpsPower(false);    // falls die Maps-App das GPS noch versorgt hatte

  // 1b) Einstellungen auf die SD spiegeln, solange der Bus ruhig ist (Audio
  //     gestoppt, Radio aus) und die Peripherie noch versorgt ist — so bleibt
  //     /fennek.ini aktuell und der Boot-Import überschreibt keine Änderung.
  settingsfile::exportToSd();

  // 1c) Debug-Akku-Logger: Standby-Zeile schreiben und Ring sofort nach SD
  //     spülen (Bus ist ruhig, Peripherie noch versorgt) — „vor Standby".
  BATTLOG_EVENT("Standby", "Akku %u%%", (unsigned)battery::percent());
  BATTLOG_FLUSH("Standby");

  // 2) Schlafender Fennek aufs E-Ink (bleibt stromlos stehen) — vorher den
  //    Akkustand lesen (I2C-Peripherie ist hier noch versorgt). Danach den
  //    Panel-Controller in den eigenen Deep Sleep schicken (µA statt Standby).
  s_sleepPct = battery::percent();
  s_timerWakes = 0;
  // Standby-Marker. Der Coulomb-Stand wird hier gelesen, solange der I2C-Gauge
  // noch versorgt ist und die Rails (DAC/LoRa/GPS) schon aus sind — die Differenz
  // zum ersten Timer-Wake ist damit fast reiner Schlafstrom.
  histAdd(PH_STBY_ARM, s_sleepPct, battery::milliVolts(),
          battery::remainingCapacity());
  s_sleepPhase = PH_STBY_RENDER;
  display::render(drawSleepScreen, true);
  display::hibernate();

  // 3) Peripherie-Power aus (Keyboard/Touch/Sensoren).
  board::perfPower(false);

  // 4) Auf STABILES Loslassen warten (300 ms durchgehend HIGH) — die
  //    Wake-Quelle ist "GPIO0 LOW"; Prellen oder ein halb gelöster Finger
  //    beendet den Schlaf sonst sofort wieder. Außerdem könnte der
  //    Strapping-Pin beim Reset in den Download-Modus geraten. Nach 10 s
  //    trotzdem schlafen: ein klemmender Knopf weckt dann sofort per EXT1
  //    (Vollboot → Auto-Standby, begrenzt und geloggt) statt hier endlos bei
  //    vollem Verbrauch zu kreisen.
  s_sleepPhase = PH_STBY_WAIT;
  uint32_t waitStart = millis();
  uint32_t hiSince = millis();
  for (;;) {
    if (btnPressed()) hiSince = millis();
    if (millis() - hiSince >= 300) break;
    if (millis() - waitStart >= 10000) {
      Serial.println("[PWR] Knopf bleibt LOW (klemmt?) — schlafe trotzdem");
      break;
    }
    delay(10);
  }

  // 5) SPI-CS deselektieren + Abschaltpegel über den Deep Sleep einfrieren.
  s_sleepPhase = PH_STBY_ARM;
  freezeSleepPins();

  // 6) Wake-Quellen = Knopf + Timer. Den nächsten Weckzeitpunkt JETZT berechnen
  //    (gültige Zeitzone) und absolut im RTC-RAM ablegen; armWakeups() leitet die
  //    Timer-Dauer daraus ab (min. Stunden-Takt). 0 = kein Wecker → Stunden-Takt.
  s_alarmWakeEpoch = alarmclock::nextDueEpoch(timesync::now());
  if (s_alarmWakeEpoch)
    Serial.printf("[FENNEK] Nächster Wecker in %ld s\n",
                  (long)(s_alarmWakeEpoch - timesync::now()));
  armWakeups();
  s_sleepPhase = PH_SLEEP_OK;
  esp_deep_sleep_start();
}

bool handleTimerWake() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) return false;

  // Wecker-Wake? Die Systemzeit (time()) überlebt den Schlaf, der Vergleich
  // braucht keine Zeitzone (beides UTC-Epochen). Wenn der Weckzeitpunkt erreicht
  // ist, NICHT den Minimal-Banner-Pfad fahren, sondern voll hochbooten — dann
  // klingelt alarmclock::poll() im normalen loop(). Flag löschen gegen Re-Trigger.
  if (s_alarmWakeEpoch) {
    uint32_t now = (uint32_t)time(nullptr);
    if (now + kAlarmWakeSlackSec >= s_alarmWakeEpoch) {
      s_alarmWakeEpoch = 0;
      Serial.println("[FENNEK] Wecker-Wake — fahre voll hoch zum Klingeln");
      return false;
    }
  }

  // Fallnetz: hängt der Minimalpfad (I2C, E-Ink, NVS), beißt nach 30 s der
  // Task-Watchdog (Panic-Reset → Vollboot → battlog-Zeile → Auto-Standby)
  // statt still bis 0 % zu laufen — der Pfad loggt sonst per Design nichts.
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);
  s_sleepPhase = PH_WAKE_START;

  // Minimal-Pfad: nur Power, I2C (Akku-Gauge), Settings (Sprache des
  // Hinweis-Texts) und Display — kein SD-Mount, kein Audio, keine Apps.
  // Akku-% aufs Schlafbild, dann sofort zurück in den Deep Sleep.
  board::powerOn();          // erzeugt auch g_spiMutex (render* lockt!)
  board::initBus();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);
  battery::begin();
  settings::begin();
  pinMode(PIN_USER_BTN, INPUT);

  s_sleepPct = battery::percent();
  s_timerWakes++;
  // Coulomb-Stand so früh wie möglich lesen — vor dem E-Ink-Refresh unten, damit
  // die Differenz zum vorigen Eintrag den Schlaf misst und nicht den Wake.
  histAdd(PH_WAKE_START, s_sleepPct, battery::milliVolts(),
          battery::remainingCapacity());
  Serial.printf("[FENNEK] Timer-Wake #%lu: Akku %u%% — zurück in Deep Sleep\n",
                (unsigned long)s_timerWakes, (unsigned)s_sleepPct);

  // Tiefentladeschutz: bei kritischem Akku auf Batterie den Display-Refresh
  // sparen (E-Ink-Booster ist der teuerste Teil des Wakes) — armWakeups()
  // unten stellt dann auch den Stunden-Timer ab.
  if (s_sleepPct <= kCritPct && !battery::external()) {
    Serial.println("[PWR] Akku kritisch — kein Banner-Refresh");
  } else {
    s_sleepPhase = PH_WAKE_DISPLAY;
    display::beginAfterSleep();
    if (s_timerWakes % kTimerWakeFullEvery == 0) {
      display::render(drawSleepScreen, true);   // periodisch gegen Ghosting
    } else {
      display::renderRegion(drawSleepBanner, kBannerY, kSleepImgH - kBannerY);
    }
    display::hibernate();
  }

  // Wurde der Knopf während des Minimal-Fensters gedrückt, will der Nutzer
  // aufwecken: Peripherie anlassen und normal weiterbooten.
  if (btnPressed()) {
    esp_task_wdt_delete(NULL);   // Vollboot: loop() füttert keinen WDT
    return false;
  }

  // Peripherie aus und — wie in enterStandby() — die Pegel über den Deep Sleep
  // einfrieren. powerOn() oben hat die Holds gelöst; ohne erneutes Hold würden
  // die Rails nach diesem ersten Stunden-Wake wieder floaten und Strom ziehen.
  s_sleepPhase = PH_WAKE_ARM;
  board::perfPower(false);
  freezeSleepPins();
  armWakeups();
  s_sleepPhase = PH_SLEEP_OK;
  esp_deep_sleep_start();    // kehrt nicht zurück
  return true;               // nie erreicht (beruhigt den Compiler)
}

void poll() {
  uint32_t now = millis();

  // Crash-Schleifen-Zähler löschen, sobald das System 2 min stabil läuft.
  if (s_abnormalBoots && now > 120000) s_abnormalBoots = 0;

  // --- Tiefentladeschutz: ≤5 % auf Batterie → sofort Standby -----------------
  // Bewusst VOR der Wiedergabe-Ausnahme unten: bei kritischem Akku wird auch
  // laufende Musik abgeschnitten, statt bis zur BMS-Abschaltung (2,9 V) zu
  // spielen. percent() ist spannungsbasiert; zwei Treffer im Abstand von 30 s
  // filtern Einbrüche unter Last, mv>100 filtert I2C-Fehler (liefert 0).
  static uint32_t s_critCheckMs = 0;
  static uint8_t  s_critHits    = 0;
  if (now - s_critCheckMs >= 30000) {
    s_critCheckMs = now;
    if (!battery::external() && battery::milliVolts() > 100 &&
        battery::percent() <= kCritPct) {
      if (++s_critHits >= 2) {
        Serial.println("[PWR] Akku kritisch (<=5%) — Not-Standby");
        BATTLOG_EVENT("Akku", "kritisch (%u%%) — Not-Standby",
                      (unsigned)battery::percent());
        enterStandby();          // kehrt nicht zurück
      }
    } else {
      s_critHits = 0;
    }
  }

  // --- Knopf (OBERER Seitenknopf = GPIO0): kurz = Tastensperre, lang =
  //     Standby. Der UNTERE Seitenknopf ist der Hardware-Reset (RST/EN) —
  //     für die Firmware unsichtbar, jeder Druck dort = sofortiger Reboot.
  if (btnPressed()) {
    if (s_btnDownAt == 0) {       // Flanke HIGH->LOW
      s_btnDownAt = now;
      s_armed = false;
      noteActivity();             // Knopfdruck zählt als Aktivität
    } else if (!s_armed && now - s_btnDownAt >= LONGPRESS_MS) {
      s_armed = true;             // nur einmal pro Druck
      Serial.println("[PWR] Standby via Langdruck (GPIO0)");
      enterStandby();             // Langdruck → kehrt nicht zurück
    }
  } else if (s_btnDownAt != 0) {  // Flanke LOW->HIGH (losgelassen)
    uint32_t dur = now - s_btnDownAt;
    s_btnDownAt = 0;
    if (!s_armed && dur >= SHORTPRESS_MIN_MS && dur < LONGPRESS_MS) {
      s_locked = !s_locked;       // Kurzklick → Tastensperre umschalten
      noteActivity();
    }
    s_armed = false;
  }

  // --- Auto-Standby nach Inaktivität ----------------------------------------
  uint8_t mins = settings::standbyMinutes();
  if (mins == 0) return;
  // Laufende Wiedergabe nie abschneiden (pausiert/gestoppt darf schlafen).
  audio::Status st = audio::status();
  if (st.playing && !st.paused) { s_lastActivity = now; return; }
  // Web-Dateiverwaltung am Netzteil nie automatisch einschlafen lassen: lange
  // WLAN-Transfers sollen nicht vom Idle-Timer gekappt werden (HTTP-Requests
  // zählen nicht als Aktivität). Nur am Strom — auf Akku bleibt der Standby aktiv,
  // damit ein vergessener Server den Akku nicht leersaugt. external() (DSG-Bit)
  // statt charging() (Mittelstrom>0), damit auch ein voller Akku am Kabel zählt —
  // sonst greift der Standby trotz Netzteil, sobald der Lader auf ~0 abregelt.
  webfm::State fm = webfm::state();
  if ((fm == webfm::State::RUNNING || fm == webfm::State::CONNECTING) &&
      battery::external()) {
    s_lastActivity = now;
    return;
  }
  if (now - s_lastActivity >= (uint32_t)mins * 60000UL) {
    Serial.printf("[PWR] Auto-Standby: idle %lus (Schwelle %u min)\n",
                  (unsigned long)((now - s_lastActivity) / 1000), mins);
    // Vor dem Auto-Standby (Gerät idle, Audio aus): bei veralteter Uhr zuerst
    // GPS-Zeit holen (genau, kein WLAN) — gelingt das, schläft das Gerät mit
    // satellitengenauer Uhr ein und der folgende NTP-Sync ist via gpsFresh() ein
    // No-op. Sonst NTP nachziehen, falls WLAN-Daten existieren. No-op bei guter
    // Uhr. (Beim manuellen Langdruck bewusst nicht — der Knopf reagiert sofort.)
    timesync::gpsSyncBeforeStandby();
    timesync::syncBeforeStandby();
    // Audio jetzt stoppen, damit der gerade pausierte/laufende Track via
    // noteTrackEnded() noch in die Scrobble-Queue geht — sonst sieht der
    // folgende Flush eine leere Queue (pendingCount()==0) und enterStandby()
    // legt den Track erst danach in den PSRAM-Ring, der im Deep Sleep verloren
    // geht (weder hochgeladen noch nach scrobbles.tsv gesichert).
    audio::stop();
    delay(60);                 // Audio-Task das Stop-Kommando abarbeiten lassen
    // Gespielte Tracks gesammelt an Navidrome scrobbeln (verwaltet WLAN selbst,
    // mit Back-off). No-op ohne offene Einträge bzw. ohne Konfiguration.
    scrobble::flushBeforeStandby();
    // Geaenderte Tagesnotizen (ausser heute) per Ollama "schoenschreiben"
    // (verwaltet WLAN selbst, mit Back-off). No-op ohne offene Notizen bzw.
    // ohne Konfiguration.
    notes_ai::flushBeforeStandby();
    // Podcast deaktiviert (WLAN-Sync zu langsam, v2.5.9): kein Auto-Sync vor Standby
    // mehr (brachte bei jedem Standby das WLAN hoch). Reaktivierbar.
    // podcast::flushBeforeStandby();
    // Todo (Reinschrift/WebDAV) hochladen + Kalender-Feeds ziehen (beide
    // verwalten WLAN selbst, mit Back-off; No-op ohne Konfig/Auto-Sync).
    reinschrift_svc::flushBeforeStandby();
    calendar::flushBeforeStandby();
    // Neue Buecher vom Calibre-Server ziehen (verwaltet WLAN selbst; hoechstens
    // alle 6 h + Back-off — bringt NICHT jeden Standby das WLAN hoch).
    calibre_books::flushBeforeStandby();
    enterStandby();
  }
}

}  // namespace power
