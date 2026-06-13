// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "board.h"
#include "config.h"

#include <Arduino.h>
#include <SD.h>

SPIClass         g_spi(HSPI);
SemaphoreHandle_t g_spiMutex = nullptr;

namespace {
bool s_sdReady = false;
}

namespace board {

void powerOn() {
  if (!g_spiMutex) g_spiMutex = xSemaphoreCreateMutex();

  // 1) Peripherie-Power (Keyboard/Touch/Sensoren) — muss zuerst kommen.
  pinMode(PIN_PERF_POWERON, OUTPUT);
  digitalWrite(PIN_PERF_POWERON, HIGH);
  delay(50);

  // 2) E-Ink-Reset hochziehen (muss HIGH sein).
  pinMode(PIN_EINK_RST, OUTPUT);
  digitalWrite(PIN_EINK_RST, HIGH);

  // 3) LoRa bleibt aus (ungenutzt), CS aber deselektieren, damit der SX1262
  //    nicht die SD-Antworten auf der gemeinsamen MISO-Leitung stört.
  pinMode(PIN_LORA_EN, OUTPUT);
  digitalWrite(PIN_LORA_EN, LOW);

  // 4) Alle SPI-CS deselektieren (HIGH) vor jeglichem Buszugriff.
  pinMode(PIN_EINK_CS, OUTPUT);  digitalWrite(PIN_EINK_CS, HIGH);
  pinMode(PIN_LORA_CS, OUTPUT);  digitalWrite(PIN_LORA_CS, HIGH);
  pinMode(PIN_SD_CS,   OUTPUT);  digitalWrite(PIN_SD_CS,   HIGH);

  // 5) DAC zunächst aus (Strom sparen, erst bei Wiedergabe an).
  pinMode(PIN_DAC_EN, OUTPUT);
  digitalWrite(PIN_DAC_EN, LOW);
}

void initBus() {
  g_spi.begin(PIN_SPI_SCLK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_EINK_CS);
}

bool initSD() {
  // SD-Karten brauchen >74 Taktzyklen nach Power-Up; CS sicher deselektiert.
  digitalWrite(PIN_EINK_CS, HIGH);
  digitalWrite(PIN_LORA_CS, HIGH);
  digitalWrite(PIN_SD_CS,   HIGH);
  delay(100);

  spiLock();
  s_sdReady = false;
  for (int attempt = 0; attempt < 3 && !s_sdReady; attempt++) {
    if (attempt > 0) {
      digitalWrite(PIN_SD_CS, HIGH);
      delay(250);
    }
    s_sdReady = SD.begin(PIN_SD_CS, g_spi, SPI_BUS_HZ);
  }
  spiUnlock();
  return s_sdReady;
}

bool sdReady() { return s_sdReady; }

void perfPower(bool on) {
  digitalWrite(PIN_PERF_POWERON, on ? HIGH : LOW);
}

void dacPower(bool on) {
  digitalWrite(PIN_DAC_EN, on ? HIGH : LOW);
}

void loraPower(bool on) {
  digitalWrite(PIN_LORA_EN, on ? HIGH : LOW);
  if (on) delay(10);   // Modul hochfahren lassen
}

}  // namespace board
