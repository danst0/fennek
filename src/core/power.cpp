#include "power.h"
#include "config.h"
#include "core/board.h"
#include "core/display.h"
#include "core/gui.h"
#include "core/settings.h"
#include "core/sleep_img.h"
#include "services/audio.h"

#include <Arduino.h>
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

uint32_t s_lastActivity = 0;
uint32_t s_btnDownAt    = 0;     // 0 = Knopf nicht gedrückt
bool     s_armed        = false; // Langdruck dieses Drucks schon ausgelöst?
bool     s_locked       = false; // Tastensperre aktiv?

bool btnPressed() { return digitalRead(PIN_USER_BTN) == BTN_PRESSED_LEVEL; }

// Standby-Bild: ein schlafender Fennek (KI-generiert, siehe sleep_img.h) als
// vollflächige 1-Bit-Bitmap, darunter ein Banner mit Titel + Aufweck-Hinweis.
void drawSleepScreen(Adafruit_GFX& g) {
  g.fillScreen(GxEPD_WHITE);
  g.drawBitmap(0, 0, kSleepImg, kSleepImgW, kSleepImgH, GxEPD_BLACK);

  // Banner unten: schwarzes Band mit weißem Text (Bild ist dort dunkel).
  const int bannerY = 278;
  g.fillRect(0, bannerY, kSleepImgW, kSleepImgH - bannerY, GxEPD_BLACK);
  g.drawFastHLine(0, bannerY, kSleepImgW, GxEPD_WHITE);
  g.setTextColor(GxEPD_WHITE);
  gui::printAt(g, 120 - 53, bannerY + 6,  "Fennek", 3);
  gui::printAt(g, 120 - 81, bannerY + 31, "Knopf drücken zum Aufwecken", 1);
  g.setTextColor(GxEPD_BLACK);
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

  // 2) Schlafender Fennek aufs E-Ink (bleibt stromlos stehen).
  display::render(drawSleepScreen, true);

  // 3) Peripherie-Power aus (Keyboard/Touch/Sensoren).
  board::perfPower(false);

  // 4) Auf Loslassen warten — sonst weckt der Wake sofort wieder bzw. der
  //    Strapping-Pin könnte beim Reset in den Download-Modus geraten.
  while (btnPressed()) delay(10);
  delay(50);                 // Entprellen

  // 5) Wake-Quelle = Knopf, dann schlafen (kehrt nicht zurück).
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_USER_BTN, WAKE_LEVEL);
  esp_deep_sleep_start();
}

void poll() {
  uint32_t now = millis();

  // --- Knopf: kurz = Tastensperre, lang = Standby ----------------------------
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
