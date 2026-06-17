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
#include "services/scrobble.h"
#include "services/settingsfile.h"
#include "services/timesync.h"
#include "services/webfm.h"

#include <Arduino.h>
#include <Wire.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <esp_sleep.h>

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
void armWakeups() {
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_USER_BTN, WAKE_LEVEL);
  esp_sleep_enable_timer_wakeup(nextWakeUs());
}

}  // namespace

namespace power {

void begin() {
  pinMode(PIN_USER_BTN, INPUT);   // externer Pull-up am Strapping-Pin
  s_lastActivity = millis();
}

void noteActivity() { s_lastActivity = millis(); }

bool locked() { return s_locked; }

void enterStandby() {
  Serial.println("[FENNEK] Standby — gehe in Deep Sleep ...");

  // Aktuelle Uhrzeit ins NVS sichern (Fallback, falls der Akku im Schlaf stirbt;
  // die ESP32-Systemzeit selbst überlebt den Deep Sleep ohne Zutun).
  uint32_t t = timesync::now();
  if (t > 1781000000UL) settings::setLastTime(t);

  // 1) Audio + DAC + Radio stilllegen.
  audio::stop();
  delay(60);                 // Audio-Task das Kommando abarbeiten lassen
  board::dacPower(false);
  board::loraPower(false);

  // 1b) Einstellungen auf die SD spiegeln, solange der Bus ruhig ist (Audio
  //     gestoppt, Radio aus) und die Peripherie noch versorgt ist — so bleibt
  //     /fennek.ini aktuell und der Boot-Import überschreibt keine Änderung.
  settingsfile::exportToSd();

  // 1c) Debug-Akku-Logger: Standby-Zeile schreiben und Ring sofort nach SD
  //     spülen (Bus ist ruhig, Peripherie noch versorgt) — „vor Standby".
  BATTLOG_EVENT("Standby", "Akku %u%%", (unsigned)battery::percent());
  BATTLOG_FLUSH("Standby");

  // 2) Schlafender Fennek aufs E-Ink (bleibt stromlos stehen) — vorher den
  //    Akkustand lesen (I2C-Peripherie ist hier noch versorgt).
  s_sleepPct = battery::percent();
  s_timerWakes = 0;
  display::render(drawSleepScreen, true);

  // 3) Peripherie-Power aus (Keyboard/Touch/Sensoren).
  board::perfPower(false);

  // 4) Auf STABILES Loslassen warten (300 ms durchgehend HIGH) — die
  //    Wake-Quelle ist "GPIO0 LOW"; Prellen oder ein halb gelöster Finger
  //    beendet den Schlaf sonst sofort wieder. Außerdem könnte der
  //    Strapping-Pin beim Reset in den Download-Modus geraten.
  uint32_t hiSince = millis();
  for (;;) {
    if (btnPressed()) hiSince = millis();
    if (millis() - hiSince >= 300) break;
    delay(10);
  }

  // 5) SPI-CS sauber deselektieren und alle Abschalt-/Deselect-Pegel über den
  //    Deep Sleep einfrieren. Ohne dieses Hold floaten GPIO10/41/46 hochohmig,
  //    die Rails (Peripherie/DAC/LoRa) kommen zurück → ~50 mA Standby-Drain
  //    (gemessen: 94 %→5 % in ~29 h, battery.log 14./15.06.2026).
  digitalWrite(PIN_EINK_CS, HIGH);
  digitalWrite(PIN_LORA_CS, HIGH);
  digitalWrite(PIN_SD_CS,   HIGH);
  board::holdSleepPins();

  // 6) Wake-Quellen = Knopf + Timer. Den nächsten Weckzeitpunkt JETZT berechnen
  //    (gültige Zeitzone) und absolut im RTC-RAM ablegen; armWakeups() leitet die
  //    Timer-Dauer daraus ab (min. Stunden-Takt). 0 = kein Wecker → Stunden-Takt.
  s_alarmWakeEpoch = alarmclock::nextDueEpoch(timesync::now());
  if (s_alarmWakeEpoch)
    Serial.printf("[FENNEK] Nächster Wecker in %ld s\n",
                  (long)(s_alarmWakeEpoch - timesync::now()));
  armWakeups();
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
  Serial.printf("[FENNEK] Timer-Wake #%lu: Akku %u%% — zurück in Deep Sleep\n",
                (unsigned long)s_timerWakes, (unsigned)s_sleepPct);

  display::beginAfterSleep();
  if (s_timerWakes % kTimerWakeFullEvery == 0) {
    display::render(drawSleepScreen, true);   // periodisch gegen Ghosting
  } else {
    display::renderRegion(drawSleepBanner, kBannerY, kSleepImgH - kBannerY);
  }

  // Wurde der Knopf während des Minimal-Fensters gedrückt, will der Nutzer
  // aufwecken: Peripherie anlassen und normal weiterbooten.
  if (btnPressed()) return false;

  // Peripherie aus und — wie in enterStandby() — die Pegel über den Deep Sleep
  // einfrieren. powerOn() oben hat die Holds gelöst; ohne erneutes Hold würden
  // die Rails nach diesem ersten Stunden-Wake wieder floaten und Strom ziehen.
  board::perfPower(false);
  digitalWrite(PIN_EINK_CS, HIGH);
  digitalWrite(PIN_LORA_CS, HIGH);
  digitalWrite(PIN_SD_CS,   HIGH);
  board::holdSleepPins();
  armWakeups();
  esp_deep_sleep_start();    // kehrt nicht zurück
  return true;               // nie erreicht (beruhigt den Compiler)
}

void poll() {
  uint32_t now = millis();

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
  // damit ein vergessener Server den Akku nicht leersaugt. Hinweis: bei vollem
  // Akku kann charging() auf 0 fallen (Strom ~0) und der Standby greift doch.
  webfm::State fm = webfm::state();
  if ((fm == webfm::State::RUNNING || fm == webfm::State::CONNECTING) &&
      battery::charging()) {
    s_lastActivity = now;
    return;
  }
  if (now - s_lastActivity >= (uint32_t)mins * 60000UL) {
    // Vor dem Auto-Standby (Gerät idle, Audio aus): wenn die Uhr-Qualität
    // schlecht ist und WLAN-Daten existieren, kurz NTP nachziehen — so schläft
    // das Gerät mit frischer Uhr ein. No-op sonst. (Beim manuellen Langdruck
    // bewusst nicht — der Knopf soll sofort reagieren.)
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
    enterStandby();
  }
}

}  // namespace power
