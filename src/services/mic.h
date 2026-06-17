// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// mic.h — PDM-Mikrofon-Aufnahme nach WAV (Audionotizen).
//
// Nutzt den zweiten I2S-Controller (I2S_NUM_1) im PDM-RX-Modus (CLK 18 / DATA 17),
// getrennt vom DAC-Ausgang (I2S_NUM_0). 16 kHz mono 16-bit reicht für Sprache.
// poll() liest DMA-Häppchen und schreibt sie gechunkt unter spiLock auf die SD;
// die fertige WAV ist über die normale Audio-Engine abspielbar.
//
// Regel: vor der Aufnahme audio::stop() (DAC frei, SPI-Bus ruhig).
// =============================================================================
#pragma once

#include <stdint.h>

namespace mic {

bool startRecording(const char* wavPath);  // I2S-PDM-RX einrichten + WAV anlegen
void poll();                               // häufig aufrufen, solange aufgenommen wird
uint32_t stopRecording();                  // WAV finalisieren; Dauer in Sekunden

bool     recording();
uint32_t recordedSec();   // bisher aufgenommene Sekunden
uint16_t level();         // letzter Spitzenpegel (0..32767) für den Pegel-Balken

}  // namespace mic
