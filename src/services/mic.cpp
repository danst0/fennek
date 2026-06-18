// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "mic.h"
#include "config.h"
#include "core/board.h"   // spiLock/spiUnlock

#include <Arduino.h>
#include <SD.h>
#include <driver/i2s.h>
#include <string.h>

namespace {

constexpr i2s_port_t kPort = I2S_NUM_0;   // PDM geht nur auf I2S0 (Handover vom DAC)
constexpr uint32_t   kRate = 16000;       // 16 kHz mono 16-bit

// PDM geht NUR auf I2S0 (IDF-Limit), das sonst der DAC belegt. Der Aufrufer MUSS
// vor startRecording() audio::beginMic() rufen (gibt I2S0 frei) und danach
// audio::endMic() (nimmt I2S0 zurück) — sonst kollidiert es mit der Wiedergabe.
bool     s_enabled   = true;

File     s_file;
bool     s_rec       = false;
uint32_t s_dataBytes = 0;
uint16_t s_level     = 0;
int16_t  s_buf[512];

void put32(uint8_t* d, uint32_t v) { d[0]=v; d[1]=v>>8; d[2]=v>>16; d[3]=v>>24; }
void put16(uint8_t* d, uint16_t v) { d[0]=v; d[1]=v>>8; }

void writeWavHeader(File& f, uint32_t dataBytes) {
  uint8_t h[44];
  memcpy(h, "RIFF", 4);   put32(h + 4, 36 + dataBytes);  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); put32(h + 16, 16);
  put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, kRate);
  put32(h + 28, kRate * 2); put16(h + 32, 2); put16(h + 34, 16);
  memcpy(h + 36, "data", 4); put32(h + 40, dataBytes);
  f.write(h, 44);
}

}  // namespace

namespace mic {

bool startRecording(const char* path) {
  if (s_rec) return false;
  if (!s_enabled) {
    Serial.println("[MIC] Aufnahme deaktiviert: PDM braucht I2S0 (vom DAC belegt) "
                   "— Audio-Engine-Handover noch nicht implementiert");
    return false;
  }
  if (!board::sdReady()) return false;

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  cfg.sample_rate = kRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;     // ~512 ms Puffer, überbrückt E-Ink-Refresh-SPI-Pausen
  cfg.dma_buf_len = 1024;
  cfg.use_apll = false;
  if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  // WICHTIG: mck NICHT auf GPIO0 lassen! Das per {}-Init gesetzte mck_io_num=0
  // würde den I2S-MCLK auf GPIO0 (= Standby-Knopf PIN_USER_BTN) legen — die
  // Langdruck-Erkennung schlägt dann mitten in der Aufnahme an und schläft ein.
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = I2S_PIN_NO_CHANGE;
  pins.ws_io_num    = PIN_MIC_CLK;        // PDM-Takt
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = PIN_MIC_DATA;       // PDM-Daten
  if (i2s_set_pin(kPort, &pins) != ESP_OK) { i2s_driver_uninstall(kPort); return false; }
  i2s_set_clk(kPort, kRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

  spiLock();
  s_file = SD.open(path, FILE_WRITE);
  if (s_file) writeWavHeader(s_file, 0);
  spiUnlock();
  if (!s_file) { i2s_driver_uninstall(kPort); return false; }

  s_dataBytes = 0;
  s_level = 0;
  s_rec = true;
  return true;
}

void poll() {
  if (!s_rec) return;
  // Mehrfach lesen: nach einer SPI-Pause (E-Ink-Refresh) den DMA-Backlog abbauen.
  for (int iter = 0; iter < 16; iter++) {
    size_t got = 0;
    i2s_read(kPort, s_buf, sizeof(s_buf), &got, 0);   // non-blocking
    if (got == 0) break;

    int n = (int)(got / 2);
    int16_t pk = 0;
    for (int i = 0; i < n; i++) { int16_t a = s_buf[i] < 0 ? -s_buf[i] : s_buf[i]; if (a > pk) pk = a; }
    if (pk > s_level || iter == 0) s_level = pk;

    spiLock();
    s_file.write((const uint8_t*)s_buf, got);
    spiUnlock();
    s_dataBytes += got;
  }
}

uint32_t stopRecording() {
  if (!s_rec) return 0;
  s_rec = false;
  i2s_driver_uninstall(kPort);

  uint8_t b[4];
  spiLock();
  s_file.seek(4);  put32(b, 36 + s_dataBytes); s_file.write(b, 4);   // RIFF-Größe
  s_file.seek(40); put32(b, s_dataBytes);       s_file.write(b, 4);   // data-Größe
  s_file.close();
  spiUnlock();
  return s_dataBytes / (kRate * 2);
}

bool     recording()   { return s_rec; }
uint32_t recordedSec() { return s_dataBytes / (kRate * 2); }
uint16_t level()       { return s_level; }

}  // namespace mic
