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
#define FENNEK_VERSION "v2.7.4"

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
// GPS-Modul (UART RX44/TX43, PPS GPIO1). Enable GPIO39 ist active-high; beim Boot
// treibt powerOn() ihn LOW (und hält ihn über den Deep Sleep), damit der
// Empfänger nicht ungenutzt Strom zieht. Die Maps-App schaltet das Modul nur
// solange sie im Vordergrund ist über board::gpsPower() ein (NMEA via UART1).
#define PIN_GPS_EN        39
#define PIN_GPS_RX        44   // GPS-TX -> ESP-RX (UART1)
#define PIN_GPS_TX        43   // GPS-RX <- ESP-TX (UART1)
#define GPS_BAUD          38400 // am Gerät verifiziert (u-blox GN-Talker, 17.06.2026)

// -----------------------------------------------------------------------------
// Vibrationsmotor-Treiber DRV2605 (V1.1, I2C 0x5A) mit Enable GPIO2 — von Fennek
// ungenutzt. Wie das GPS beim Boot auf LOW (Hardware-Shutdown, ~µA) und über den
// Schlaf gehalten, damit der Treiber nicht im Standby ~1-2 mA zieht.
#define PIN_DRV_EN        2

// -----------------------------------------------------------------------------
// Tastatur-Backlight (GPIO42). Auf dem T-Deck Pro MAX bestückt; auf manchen
// Boards evtl. nicht angeschlossen — GPIO42 ist sonst frei, das Ansteuern ist
// also gefahrlos (tut dann nichts). Wird als Wecker-Sichtsignal geblinkt.
#define PIN_KB_BL         42

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

// PDM-Mikrofon (Audionotizen). PDM geht laut IDF nur auf I2S0 — das belegt sonst
// der DAC, daher der Handover in audio::beginMic/endMic (gibt I2S0 frei, nimmt es
// danach zurück). CLK = GPIO18, DATA = GPIO17 (variant.h).
#define PIN_MIC_CLK       18
#define PIN_MIC_DATA      17

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
