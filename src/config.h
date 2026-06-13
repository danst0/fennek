// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// config.h — zentrale Hardware-Konstanten für den T-Deck Pro MP3-Player.
//
// Alle Pin-Belegungen sind aus dem (jetzt archivierten) hardware-verifizierten
// MeshCore-Code für die T-Deck Pro V1.1 übernommen:
//   archive_legacy/variants/lilygo_tdeck_pro/variant.h
// =============================================================================
#pragma once

#include <stdint.h>

// Firmware-Version (wird u. a. in der Optionen-App angezeigt).
#define FENNEK_VERSION "v1.6.3"

// -----------------------------------------------------------------------------
// Peripherie-Power
// -----------------------------------------------------------------------------
#define PIN_PERF_POWERON  10   // schaltet Keyboard, Touch, Sensoren ein
#define PIN_DAC_EN        41   // BOARD_6609_EN: Versorgung des PCM5102A-DAC

// -----------------------------------------------------------------------------
// Geteilter SPI-Bus (E-Ink + LoRa + SD) — HSPI
// -----------------------------------------------------------------------------
#define PIN_SPI_SCLK      36
#define PIN_SPI_MISO      47
#define PIN_SPI_MOSI      33
#define SPI_BUS_HZ        4000000UL   // 4 MHz: auf der Hardware stabil (Writes!)

// -----------------------------------------------------------------------------
// E-Ink GDEQ031T10 (240x320, monochrom)
// -----------------------------------------------------------------------------
#define PIN_EINK_CS       34
#define PIN_EINK_DC       35
#define PIN_EINK_RST      16   // muss HIGH gehalten werden
#define PIN_EINK_BUSY     37
#define EINK_W            240
#define EINK_H            320

// -----------------------------------------------------------------------------
// SD-Karte (CS am geteilten SPI-Bus)
// -----------------------------------------------------------------------------
#define PIN_SD_CS         48

// -----------------------------------------------------------------------------
// LoRa SX1262 (Mesh-App) — CS muss deselektiert (HIGH) sein, wenn inaktiv,
// sonst stört der SX1262 die SD-Antworten auf der gemeinsamen MISO-Leitung.
// Pins aus archive_legacy/variants/lilygo_tdeck_pro/variant.h.
// -----------------------------------------------------------------------------
#define PIN_LORA_CS        3
#define PIN_LORA_EN       46
#define PIN_LORA_RST       4
#define PIN_LORA_DIO1      5
#define PIN_LORA_BUSY      6

// -----------------------------------------------------------------------------
// I2C (Touch + Keyboard + Sensoren) — getrennt vom SPI-Bus!
// -----------------------------------------------------------------------------
#define PIN_I2C_SDA       13
#define PIN_I2C_SCL       14
#define I2C_HZ            100000UL

// Touch CST328 (Hynitron-Treiber)
#define PIN_TOUCH_INT     12
#define PIN_TOUCH_RST     38
#define TOUCH_ADDR        0x1A

// -----------------------------------------------------------------------------
// Audio I2S → PCM5102A DAC
// -----------------------------------------------------------------------------
#define PIN_I2S_BCLK       7
#define PIN_I2S_DOUT       8
#define PIN_I2S_LRC        9

// -----------------------------------------------------------------------------
// Wiedergabe-Parameter
// -----------------------------------------------------------------------------
#define AUDIO_VOL_MAX      21   // Wertebereich der ESP32-audioI2S-Lib
#define AUDIO_VOL_DEFAULT  12
#define MP3_DIR            "/music"   // Standard-Verzeichnis auf der SD
// Track-Puffer wachsen bedarfsbasiert in Blöcken (kein praktisches Limit;
// Speicher = tatsächliche Sammlungsgröße). TRACKS_HARD_MAX ist nur ein
// Überlauf-Schutz gegen Amok-Scans (Abspiel-Reihenfolge ist uint16_t).
#define TRACK_BLOCK        512        // Einträge pro Block (Zweierpotenz)
#define TRACKS_HARD_MAX    32768
// Calibre-artige Pfade (/music/Künstler/Album (Jahr)/Disc/Langer Titel.mp3)
// sprengen 192 Zeichen; zu lange Pfade werden geloggt und übersprungen.
#define TRACK_PATH_LEN     256
