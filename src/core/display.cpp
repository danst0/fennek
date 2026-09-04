// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "display.h"
#include "board.h"
#include "config.h"
#include "settings.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <GxEPD2_BW.h>

// EINK_DISPLAY_MODEL kommt aus platformio.ini (= GxEPD2_310_GDEQ031T10).
// Voller Frame-Buffer (page_height == HEIGHT): 240*320/8 = 9600 Bytes.
static GxEPD2_BW<EINK_DISPLAY_MODEL, EINK_DISPLAY_MODEL::HEIGHT> g_disp(
    EINK_DISPLAY_MODEL(PIN_EINK_CS, PIN_EINK_DC, PIN_EINK_RST, PIN_EINK_BUSY));

namespace {
// Nach diesem Intervall an Partial-Refreshes wird ein Full-Refresh eingestreut,
// um Ghosting zu beseitigen (vgl. Legacy EINK_LIMIT_FASTREFRESH=10).
constexpr int kFullRefreshEvery = 10;
int  s_partialCount = 0;
bool s_initialized  = false;
// Panel-Init an klemmender BUSY-Leitung gescheitert -> Hardware-Reset wirkt
// auf diesem Board nicht. hibernate() schaltet daraufhin dauerhaft ab.
bool s_panelStuck   = false;

// RST-Puls-Längen (ms) für den Panel-Reset, eskalierend. GxEPD2-Default ist
// 10 ms, hier standen früher 2 ms — zu wenig, um ein zickiges Panel sauber
// zurückzusetzen. Der Reset ist auch der einzige Weg aus dem Controller-Deep-
// Sleep (s. hibernate() unten), deshalb fassen wir mit längeren Pulsen nach,
// falls BUSY nicht freigibt.
constexpr uint16_t kResetPulseMs[] = {20, 50, 200};
// Wartezeit auf die BUSY-Freigabe nach dem Reset (Panel meldet sich in ~10 ms).
constexpr uint32_t kReadyTimeoutMs = 500;

// Wird von GxEPD2 während der ~600 ms BUSY-Wartephase wiederholt aufgerufen.
// In dieser Phase aktualisiert der E-Ink-Controller die Anzeige INTERN — es
// läuft KEIN SPI-Verkehr. Wir geben den Bus deshalb kurz frei, damit der
// Audio-Task (Core 0) von der SD nachladen und das I2S-DMA füllen kann.
// Sonst liefe der DMA-Puffer (~90 ms) während des Refreshs leer -> Stottern.
void einkBusyWait(const void*) {
  spiUnlock();
  vTaskDelay(1);   // dem Audio-Task einen Zug auf dem Bus geben
  spiLock();
}

// BUSY ist beim GDEQ031T10 active-LOW: HIGH = Panel bereit.
bool panelReady(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (digitalRead(PIN_EINK_BUSY) == LOW) {
    if (millis() - t0 > timeoutMs) return false;
    delay(2);
  }
  return true;
}

// init() mit eskalierender Reset-Puls-Länge, bis das Panel BUSY freigibt.
// `initial` = GxEPD2-Semantik: true verwirft den stehenden Bildinhalt,
// false setzt auf dem angezeigten Bild auf (Wake-from-Deep-Sleep-Muster).
bool initPanel(bool initial) {
  for (uint16_t ms : kResetPulseMs) {
    g_disp.init(115200, initial, ms, false);
    if (panelReady(kReadyTimeoutMs)) return true;
    Serial.printf("[EINK] BUSY bleibt aktiv nach %u-ms-Reset — neuer Versuch\n",
                  (unsigned)ms);
  }
  Serial.println("[EINK] Panel antwortet nicht (BUSY aktiv) — fahre trotzdem fort");
  s_panelStuck = true;
  return false;
}
}  // namespace

namespace display {

void begin() {
  spiLock();
  // Geteilten Bus + 4 MHz für zuverlässige E-Ink-Kommunikation verwenden.
  g_disp.epd2.selectSPI(g_spi, SPISettings(SPI_BUS_HZ, MSBFIRST, SPI_MODE0));
  initPanel(true);
  // Bus während der Panel-BUSY-Phase für den Audio-Task freigeben.
  g_disp.epd2.setBusyCallback(einkBusyWait);
  g_disp.setRotation(0);
  g_disp.setTextColor(GxEPD_BLACK);
  g_disp.setTextWrap(false);
  // CP437-Glyphensatz aktivieren — enthält ä/ö/ü/ß (Umlaute via gui::toCp437).
  g_disp.cp437(true);
  g_disp.setFullWindow();
  g_disp.firstPage();
  do {
    g_disp.fillScreen(GxEPD_WHITE);
  } while (g_disp.nextPage());
  g_disp.powerOff();
  spiUnlock();
  s_initialized = true;
}

void beginAfterSleep() {
  spiLock();
  g_disp.epd2.selectSPI(g_spi, SPISettings(SPI_BUS_HZ, MSBFIRST, SPI_MODE0));
  // initial=false (GxEPD2-Muster „wake from deep sleep"): der Controller wird
  // initialisiert, ohne den stehenden Bildinhalt zu verwerfen — Partial-
  // Refreshes arbeiten danach gegen das angezeigte Schlafbild.
  initPanel(false);
  g_disp.epd2.setBusyCallback(einkBusyWait);
  g_disp.setRotation(0);
  g_disp.setTextColor(GxEPD_BLACK);
  g_disp.setTextWrap(false);
  g_disp.cp437(true);
  spiUnlock();
  s_initialized = true;
}

void render(DrawFn draw, bool full) {
  if (!s_initialized) return;

  if (s_partialCount >= kFullRefreshEvery) full = true;

  spiLock();
  if (full) {
    g_disp.setFullWindow();
    s_partialCount = 0;
  } else {
    g_disp.setPartialWindow(0, 0, g_disp.width(), g_disp.height());
    s_partialCount++;
  }
  g_disp.firstPage();
  do {
    g_disp.fillScreen(GxEPD_WHITE);
    draw(g_disp);
  } while (g_disp.nextPage());
  // Booster abschalten — E-Ink hält das Bild ohne Strom (EINK_POWER_OFF_IDLE).
  g_disp.powerOff();
  spiUnlock();
}

void renderRegion(DrawFn draw, int y, int h) {
  if (!s_initialized) return;
  spiLock();
  // Nur diesen Streifen partiell aktualisieren (volle Breite, 8er-ausgerichtet).
  g_disp.setPartialWindow(0, y, g_disp.width(), h);
  g_disp.firstPage();
  do {
    g_disp.fillScreen(GxEPD_WHITE);  // wirkt nur im Fenster
    draw(g_disp);
  } while (g_disp.nextPage());
  g_disp.powerOff();
  spiUnlock();
}

void hibernate() {
  if (!s_initialized) return;

  // GxEPD2::hibernate() = powerOff + Kommando 0x07: der UC8253 im GDEQ031T10
  // geht in den Controller-Deep-Sleep und kommt dort NUR über einen Hardware-
  // Reset wieder heraus. Auf der T-Deck Pro V1.0 gibt es diesen Weg nicht — die
  // RST-Leitung zum EPD ist gar nicht verdrahtet (LilyGO definiert für V1.0
  // BOARD_EPD_RST -1; GPIO 16 ist dort der Helligkeitssensor-Interrupt). Das
  // Panel bleibt dann firmwareübergreifend mit aktivem BUSY stehen, bis es sich
  // nach Stunden selbst entladen hat — genau so in GitHub-Issue #2 gesehen.
  // Keine Puls-Länge der Welt hilft dagegen, also senden wir 0x07 per Default
  // nicht mehr: powerOff() schaltet den Booster ab, das Bild steht auch so
  // stromlos weiter, und der Aufwach-Pfad braucht keinen Reset.
  //
  // Selbstheilung: Ist das Panel schon einmal nicht zurückgekommen, ist der
  // Reset auf dieser Hardware wirkungslos — dann das Flag dauerhaft löschen.
  if (s_panelStuck && settings::einkHibernate()) {
    settings::setEinkHibernate(false);
    Serial.println("[EINK] Panel kam nicht aus dem Deep-Sleep — hibernate dauerhaft aus");
  }
  const bool deepSleep = settings::einkHibernate() && !s_panelStuck;

  spiLock();
  if (deepSleep) g_disp.hibernate();   // 0x07: µA, Wake nur via RST
  else           g_disp.powerOff();    // Booster aus, Controller bleibt wach
  spiUnlock();
}

bool panelStuck() { return s_panelStuck; }

int width()  { return g_disp.width(); }
int height() { return g_disp.height(); }

}  // namespace display
