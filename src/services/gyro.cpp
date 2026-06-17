// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "gyro.h"
#include "config.h"

#include <Arduino.h>
#include <Wire.h>
#include "SensorBHI260AP.hpp"   // lewisxhe/SensorLib (bringt das BHy2-Firmware-Blob mit)

namespace {

SensorBHI260AP s_bhy;
gyro::Data     s_data   = {};
bool           s_inited = false;   // Firmware schon hochgeladen?

// Callbacks feuern synchron aus s_bhy.update() (gleicher Thread) — kein Lock nötig.
void accelCb(uint8_t id, uint8_t* data, uint32_t, uint64_t*) {
  bhy2_data_xyz v;
  bhy2_parse_xyz(data, &v);
  float sf = get_sensor_default_scaling(id);
  s_data.ax = v.x * sf; s_data.ay = v.y * sf; s_data.az = v.z * sf;
}
void gyroCb(uint8_t id, uint8_t* data, uint32_t, uint64_t*) {
  bhy2_data_xyz v;
  bhy2_parse_xyz(data, &v);
  float sf = get_sensor_default_scaling(id);
  s_data.gx = v.x * sf; s_data.gy = v.y * sf; s_data.gz = v.z * sf;
}

}  // namespace

namespace gyro {

bool begin() {
  if (!s_inited) {
    s_bhy.setPins(-1, -1);   // kein Reset/IRQ -> update() pollt das FIFO
    s_bhy.setFirmware(bhy2_firmware_image, sizeof(bhy2_firmware_image));
    Serial.println("[GYRO] BHI260 Firmware-Upload (~10 s) ...");
    if (!s_bhy.init(Wire, PIN_I2C_SDA, PIN_I2C_SCL, BHI260AP_SLAVE_ADDRESS_L)) {
      Serial.printf("[GYRO] init fehlgeschlagen (Fehlercode %d)\n", s_bhy.getError());
      s_data.ok = false;
      return false;
    }
    s_bhy.onResultEvent(SENSOR_ID_ACC_PASS,  accelCb);
    s_bhy.onResultEvent(SENSOR_ID_GYRO_PASS, gyroCb);
    s_inited = true;
    Serial.println("[GYRO] BHI260 bereit");
  }
  s_bhy.configure(SENSOR_ID_ACC_PASS,  50.0f, 0);   // 50 Hz
  s_bhy.configure(SENSOR_ID_GYRO_PASS, 50.0f, 0);
  s_data.ok = true;
  return true;
}

void end() {
  if (s_inited) {
    s_bhy.configure(SENSOR_ID_ACC_PASS,  0.0f, 0);  // 0 Hz = Leerlauf
    s_bhy.configure(SENSOR_ID_GYRO_PASS, 0.0f, 0);
  }
  s_data.ok = false;
}

void poll() {
  if (s_data.ok) s_bhy.update();
}

const Data& current() { return s_data; }

}  // namespace gyro
