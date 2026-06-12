#include "power.h"
#include "config.h"
#include "core/battery.h"
#include "core/board.h"
#include "core/display.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "core/settings.h"
#include "core/sleep_img.h"
#include "services/audio.h"

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

bool btnPressed() { return digitalRead(PIN_USER_BTN) == BTN_PRESSED_LEVEL; }

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
  esp_sleep_enable_timer_wakeup(kTimerWakeUs);
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

  // 1) Audio + DAC + Radio stilllegen.
  audio::stop();
  delay(60);                 // Audio-Task das Kommando abarbeiten lassen
  board::dacPower(false);
  board::loraPower(false);

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

  // 5) Wake-Quellen = Knopf + Stunden-Timer (Akku-%), dann schlafen
  //    (kehrt nicht zurück).
  armWakeups();
  esp_deep_sleep_start();
}

bool handleTimerWake() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) return false;

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

  board::perfPower(false);
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
  if (now - s_lastActivity >= (uint32_t)mins * 60000UL) {
    enterStandby();
  }
}

}  // namespace power
