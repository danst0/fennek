#include "display.h"
#include "board.h"
#include "config.h"

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
}  // namespace

namespace display {

void begin() {
  spiLock();
  // Geteilten Bus + 4 MHz für zuverlässige E-Ink-Kommunikation verwenden.
  g_disp.epd2.selectSPI(g_spi, SPISettings(SPI_BUS_HZ, MSBFIRST, SPI_MODE0));
  g_disp.init(115200, true, 2, false);
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

int width()  { return g_disp.width(); }
int height() { return g_disp.height(); }

}  // namespace display
