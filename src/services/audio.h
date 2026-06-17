// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// audio.h — Wiedergabe (ESP32-audioI2S → PCM5102A), pfadbasierte Queue.
//
// Anti-Stotter-Architektur (unverändert):
//   - Dekodierung läuft in einem EIGENEN FreeRTOS-Task auf Core 0; die UI
//     läuft auf Core 1 → keine CPU-Konkurrenz.
//   - audio.loop() läuft unter dem SPI-Mutex; während E-Ink-BUSY-Phasen wird
//     der Bus freigegeben (display.cpp), der 256-KB-PSRAM-Read-Ahead puffert.
//   - Alle audio.*-Aufrufe der Lib passieren NUR im Audio-Task; die UI sendet
//     Kommandos über eine Queue.
//
// Neu (Multi-App):
//   - Die Abspiel-Queue enthält SD-PFADE (nicht Library-Indizes) — damit kann
//     auch die Hörbuch-App die Pipeline füttern, ohne library.cpp zu kennen.
//   - Owner-Token (Music/Book): startet der andere Owner, wird die laufende
//     Wiedergabe gestoppt; der Verlierer sieht das per status().owner.
//   - Shuffle/Repeat, relatives Seek (CBR-MP3), Start-Offset, Sleep-Timer.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace audio {

enum class Owner  : uint8_t { None, Music, Book };
enum class Repeat : uint8_t { Off, All, One };

struct Status {
  bool     playing;
  bool     paused;
  int      pos;        // Position in der Abspiel-Queue (-1 = keine)
  int      queueLen;
  uint32_t posSec;
  uint32_t durSec;
  uint8_t  volume;     // 0..AUDIO_VOL_MAX
  Owner    owner;
  bool     shuffle;
  Repeat   repeat;
  uint16_t sleepMin;   // verbleibende Sleep-Timer-Minuten (aufgerundet), 0 = aus
};

void begin();

// --- Queue setzen (Staging, dann atomarer Commit) ----------------------------
// queueBegin/queueAdd laufen im UI-Thread (kein SD-Zugriff); queueCommit
// übergibt die Queue an den Audio-Task und startet bei startPos (+startSec).
void queueBegin(Owner owner);
bool queueAdd(const char* path);            // false wenn voll
void queueCommit(int startPos, uint32_t startSec = 0);

// Pfad des aktuell spielenden Tracks ("" wenn keiner).
void currentPath(char* out, size_t n);

void togglePause();
void next();
void prev();
void stop();

// I2S0-Handover für die PDM-Mikro-Aufnahme (services/mic): beginMic() gibt I2S0
// frei (Wiedergabe stoppt), endMic() nimmt es zurück. Während der Aufnahme
// ignoriert die Engine Play-Kommandos.
bool beginMic();
void endMic();
void seekRel(int sec);                      // ±Sekunden (zuverlässig bei CBR-MP3)
void setVolume(uint8_t v);
void volumeUp();
void volumeDown();
void setShuffle(bool on);                   // aktueller Track bleibt aktuell
void setRepeat(Repeat r);
void setSleepTimer(uint16_t minutes);       // 0 = aus; stoppt Wiedergabe bei Ablauf

Status status();

}  // namespace audio
