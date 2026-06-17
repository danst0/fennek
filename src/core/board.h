// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

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

// Peripherie-Versorgung (PIN_PERF_POWERON: Keyboard/Touch/Sensoren) schalten.
// Vor Deep-Sleep ausschalten spart Strom; powerOn() schaltet sie wieder ein.
void perfPower(bool on);

// DAC-Versorgung (PCM5102A) schalten.
void dacPower(bool on);

// LoRa-Modul-Versorgung (SX1262) schalten — vor dem Radio-Init aufrufen.
void loraPower(bool on);

// GPS-Modul (NMEA über UART1, RX44/TX43, Enable GPIO39 active-high) versorgen.
// Nur die Maps-App nutzt es, und nur solange sie vorn ist (~20-40 mA):
//   gpsPower(true)  — Enable HIGH + Serial1 starten
//   gpsPower(false) — Serial1 beenden + Enable LOW (Sparzustand wie nach powerOn)
void gpsPower(bool on);

// Vibrationsmotor (DRV2605, I2C 0x5A, Enable GPIO2). Im Leerlauf ist der Treiber
// per powerOn() in Hardware-Shutdown (Strom sparen). Für den Wecker:
//   vibraEnable(true)  — Treiber aufwecken + RTP-Modus (einmal beim Klingelstart)
//   vibrate(on)        — Motor an/aus (RTP-Stärke) zum Pulsen
//   vibraEnable(false) — Motor aus + zurück in Shutdown (beim Quittieren)
void vibraEnable(bool en);
void vibrate(bool on);

// Enable-/Deselect-Pins (Peripherie/DAC/LoRa-Power + alle CS + E-Ink-RST) über
// den Deep Sleep EINFRIEREN. Ohne das floaten die digitalen Pads beim
// esp_deep_sleep_start() hochohmig, die Rails kommen zurück (~50 mA Standby).
// MUSS vor jedem esp_deep_sleep_start() laufen — auch im Timer-Wake-Pfad.
void holdSleepPins();

// Die mit holdSleepPins() gesetzten Holds wieder lösen, damit die Pins nach dem
// Wake neu getrieben werden können. Läuft am Anfang von powerOn() (no-op beim
// Kaltstart, da dann nichts gehalten ist).
void releaseSleepPins();

}  // namespace board

// Komfort-Lock für den SPI-Bus.
static inline void spiLock()   { xSemaphoreTake(g_spiMutex, portMAX_DELAY); }
static inline void spiUnlock() { xSemaphoreGive(g_spiMutex); }
