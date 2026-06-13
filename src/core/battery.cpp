// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "battery.h"

#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr uint8_t kAddr          = 0x55;
constexpr uint8_t REG_VOLTAGE    = 0x08;
constexpr uint8_t REG_AVG_CURRENT= 0x14;   // mA, signed
constexpr uint8_t REG_SOC        = 0x2C;

bool s_ready = false;

uint16_t read16(uint8_t reg) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(kAddr, (uint8_t)2) != 2) return 0;
  uint16_t v = Wire.read();
  v |= (Wire.read() << 8);
  return v;
}

}  // namespace

namespace battery {

bool begin() {
  Wire.beginTransmission(kAddr);
  s_ready = (Wire.endTransmission() == 0);
  return s_ready;
}

uint16_t milliVolts() { return s_ready ? read16(REG_VOLTAGE) : 0; }

uint8_t percent() {
  if (!s_ready) return 0;
  uint16_t soc = read16(REG_SOC);
  return (uint8_t)(soc > 100 ? 100 : soc);
}

bool charging() {
  if (!s_ready) return false;
  int16_t mA = (int16_t)read16(REG_AVG_CURRENT);
  return mA > 0;
}

}  // namespace battery
