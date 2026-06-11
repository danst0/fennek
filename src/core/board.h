// =============================================================================
// board.h — Power-Sequenz, geteilter SPI-Bus und SD-Karte.
//
// Kernpunkt der Architektur: E-Ink und SD-Karte teilen sich denselben SPI-Bus
// (SCLK 36 / MOSI 33 / MISO 47). Audio liest MP3-Daten von der SD, das Display
// schreibt beim Refresh auf denselben Bus. Damit sich beide nicht korrumpieren,
// wird jeder Buszugriff über g_spiMutex serialisiert.
// =============================================================================
#pragma once

#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Gemeinsame HSPI-Instanz für E-Ink und SD-Karte.
extern SPIClass        g_spi;
// Serialisiert den Zugriff auf den geteilten SPI-Bus (Audio-SD-Read vs. Refresh).
extern SemaphoreHandle_t g_spiMutex;

namespace board {

// Peripherie-Power einschalten, alle SPI-CS-Leitungen deselektieren.
void powerOn();

// Geteilten SPI-Bus initialisieren (vor Display-Init aufrufen).
void initBus();

// SD-Karte am geteilten Bus mounten (mit Retry). true = bereit.
bool initSD();
bool sdReady();

// DAC-Versorgung (PCM5102A) schalten.
void dacPower(bool on);

// LoRa-Modul-Versorgung (SX1262) schalten — vor dem Radio-Init aufrufen.
void loraPower(bool on);

}  // namespace board

// Komfort-Lock für den SPI-Bus.
static inline void spiLock()   { xSemaphoreTake(g_spiMutex, portMAX_DELAY); }
static inline void spiUnlock() { xSemaphoreGive(g_spiMutex); }
