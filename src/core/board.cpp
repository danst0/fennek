// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "board.h"
#include "config.h"

#include <Arduino.h>
#include <SD.h>
#include <Wire.h>          // DRV2605 (Vibrationsmotor) über I2C
#include <driver/gpio.h>   // gpio_hold_en/dis, gpio_deep_sleep_hold_en/dis

SPIClass         g_spi(HSPI);
SemaphoreHandle_t g_spiMutex = nullptr;

namespace {
bool s_sdReady = false;

// Pins, deren Pegel über den Deep Sleep eingefroren werden müssen, damit die
// Rails (Peripherie/DAC/LoRa) aus bleiben und die SPI-CS deselektiert. GPIO10/16
// sind RTC-fähig, 41/46/34/3/48 nicht — gpio_deep_sleep_hold_en() deckt beide ab.
const gpio_num_t kHoldPins[] = {
  (gpio_num_t)PIN_PERF_POWERON, (gpio_num_t)PIN_DAC_EN, (gpio_num_t)PIN_LORA_EN,
  (gpio_num_t)PIN_EINK_CS, (gpio_num_t)PIN_LORA_CS, (gpio_num_t)PIN_SD_CS,
  (gpio_num_t)PIN_EINK_RST, (gpio_num_t)PIN_GPS_EN, (gpio_num_t)PIN_DRV_EN,
  (gpio_num_t)PIN_KB_BL,   // Backlight-Gate LOW halten (floatete sonst im Schlaf)
};
}

namespace board {

void powerOn() {
  if (!g_spiMutex) g_spiMutex = xSemaphoreCreateMutex();

  // 0) Etwaige Deep-Sleep-Holds lösen (sonst lassen sich die eingefrorenen
  //    Enable-/CS-Pins unten nicht neu treiben). Beim Kaltstart ein No-op.
  releaseSleepPins();

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

  // 6) GPS-Modul (vorhanden, aber ungenutzt) ausschalten — Enable ist active-high
  //    (GPIO39). Ohne dies floatet der Pin und der Empfänger könnte ungenutzt
  //    ~20-40 mA ziehen. Der Pegel wird über den Deep Sleep gehalten (kHoldPins).
  pinMode(PIN_GPS_EN, OUTPUT);
  digitalWrite(PIN_GPS_EN, LOW);

  // 7) Vibrationsmotor-Treiber DRV2605 (ungenutzt) in Hardware-Shutdown — EN
  //    (GPIO2) LOW, ebenfalls über den Schlaf gehalten. Spart ~1-2 mA Standby.
  pinMode(PIN_DRV_EN, OUTPUT);
  digitalWrite(PIN_DRV_EN, LOW);

  // 8) Tastatur-Backlight (GPIO42) definiert aus — der Pin trieb bisher nie
  //    jemand außer dem Wecker-Blinken; undefiniert/floatend kann das Gate
  //    des Backlight-FETs Strom ziehen. Auf Boards ohne Backlight harmlos.
  pinMode(PIN_KB_BL, OUTPUT);
  digitalWrite(PIN_KB_BL, LOW);
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

void gpsPower(bool on) {
  if (on) {
    pinMode(PIN_GPS_EN, OUTPUT);
    digitalWrite(PIN_GPS_EN, HIGH);
    delay(5);          // Empfänger-Versorgung stabilisieren lassen
    Serial1.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  } else {
    Serial1.end();
    digitalWrite(PIN_GPS_EN, LOW);   // zurück in den Sparzustand (wie powerOn)
  }
}

// --- Vibrationsmotor DRV2605 (I2C 0x5A) -----------------------------------------
namespace {
constexpr uint8_t kDrvAddr = 0x5A;
void drvWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kDrvAddr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}
}  // namespace

void vibraEnable(bool en) {
  if (en) {
    digitalWrite(PIN_DRV_EN, HIGH);   // aus Hardware-Shutdown holen
    delay(2);                         // Treiber-Boot-Zeit
    drvWrite(0x01, 0x05);             // MODE = RTP (Real-Time-Playback), kein Standby
    drvWrite(0x02, 0x00);             // Motor zunächst aus
  } else {
    drvWrite(0x02, 0x00);             // Motor aus
    drvWrite(0x01, 0x40);             // Standby
    digitalWrite(PIN_DRV_EN, LOW);    // Shutdown (Strom sparen)
  }
}

void vibrate(bool on) {
  drvWrite(0x02, on ? 0x7F : 0x00);   // RTP-Stärke (0x7F = Maximum)
}

void holdSleepPins() {
  // Aktuellen Pegel jedes Pins latchen, dann das Latch über den Deep Sleep
  // aktiv halten. gpio_hold_en deckt die RTC-Pads (10/16) ab, der
  // deep_sleep_hold zusätzlich die digitalen (41/46/34/3/48).
  for (gpio_num_t p : kHoldPins) gpio_hold_en(p);
  gpio_deep_sleep_hold_en();
}

void releaseSleepPins() {
  gpio_deep_sleep_hold_dis();
  for (gpio_num_t p : kHoldPins) gpio_hold_dis(p);
}

}  // namespace board
