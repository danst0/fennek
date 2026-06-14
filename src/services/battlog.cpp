// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "battlog.h"

// Die gesamte Übersetzungseinheit ist gegatet — ohne -D BATTLOG entsteht keine
// einzige Funktion/Symbol (die Aufrufer expandieren ohnehin zu ((void)0)).
#ifdef BATTLOG

#include "config.h"
#include "core/battery.h"
#include "core/board.h"
#include "services/timesync.h"
#include "services/webfm.h"
#include "apps/mesh_client.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

constexpr int      kRingMax     = 256;            // PSRAM-Ring (FIFO)
const char*        kFile        = "/.fennek/battery.log";
const char*        kFileOld     = "/.fennek/battery.old";
constexpr uint32_t kSampleMs    = 60000;          // periodische Akku-Probe
constexpr uint32_t kFlushMs     = 120000;         // spätestens alle 2 min nach SD
constexpr int      kFlushAt     = 200;            // oder wenn so viele Zeilen offen
constexpr uint32_t kMaxFileSize = 256UL * 1024;   // Rotation (wie das Mesh-Log)
constexpr uint32_t kSaneEpoch   = 1500000000UL;   // darunter Uhr ungültig

// Flag-Bits im Entry.flags-Feld.
constexpr uint8_t F_CHG  = 1;   // lädt
constexpr uint8_t F_WIFI = 2;   // WLAN (webfm) an
constexpr uint8_t F_MESH = 4;   // Mesh-Radio initialisiert (Chip an)

struct Entry {
  uint32_t epoch;     // UTC; 0/ungültig = Uhr noch nicht gesetzt
  uint16_t mv;        // Akkuspannung
  uint8_t  pct;       // Ladestand %
  uint8_t  flags;     // F_CHG | F_WIFI | F_MESH
  char     cat[12];   // Kategorie (Boot/App/Audio/Mesh/WLAN/Standby/Wake/Akku/…)
  char     text[64];  // Notiz
};

Entry*            s_ring   = nullptr;   // PSRAM
int               s_count  = 0;
SemaphoreHandle_t s_mutex  = nullptr;
uint32_t          s_lastSampleMs = 0;
uint32_t          s_lastFlushMs  = 0;

// Gecachte Akku-Werte: nur poll() (Core 1) liest den I2C-Gauge; event() nutzt
// den Cache, damit es aus jedem Task — auch dem Audio-Task — ohne I2C-Konkurrenz
// auf dem Wire-Bus aufrufbar bleibt.
volatile uint16_t s_cMv  = 0;
volatile uint8_t  s_cPct = 0;
volatile bool     s_cChg = false;
bool              s_haveCharge = false;   // schon mal Lade-Status gesehen?

uint8_t sysFlags(bool chg) {
  uint8_t f = 0;
  if (chg)                                 f |= F_CHG;
  if (webfm::state() != webfm::State::OFF) f |= F_WIFI;
  if (mesh_client::ready())                f |= F_MESH;   // beide nur Flag-Reads
  return f;
}

// Eintrag in den Ring legen (FIFO; voll → ältesten verwerfen). Task-sicher.
void append(const char* cat, const char* text) {
  if (!s_ring) return;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  if (s_count >= kRingMax) {
    memmove(&s_ring[0], &s_ring[1], sizeof(Entry) * (kRingMax - 1));
    s_count = kRingMax - 1;
  }
  Entry& e = s_ring[s_count++];
  e.epoch = timesync::now();
  e.mv    = s_cMv;
  e.pct   = s_cPct;
  e.flags = sysFlags(s_cChg);
  strncpy(e.cat,  cat,  sizeof(e.cat)  - 1); e.cat[sizeof(e.cat)   - 1] = '\0';
  strncpy(e.text, text, sizeof(e.text) - 1); e.text[sizeof(e.text) - 1] = '\0';
  xSemaphoreGive(s_mutex);
}

// Eine Zeile (Tab-getrennt) für die SD bauen.
void formatLine(const Entry& e, char* out, size_t n) {
  char ts[24];
  if (e.epoch >= kSaneEpoch) {
    time_t t = (time_t)e.epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
  } else {
    strncpy(ts, "0000-00-00 00:00:00", sizeof(ts));
    ts[sizeof(ts) - 1] = '\0';
  }
  snprintf(out, n, "%s\t%u%%\t%umV\t%s\tW:%s\tM:%s\t%s\t%s\n",
           ts, (unsigned)e.pct, (unsigned)e.mv,
           (e.flags & F_CHG)  ? "laedt" : "-",
           (e.flags & F_WIFI) ? "an"    : "aus",
           (e.flags & F_MESH) ? "an"    : "aus",
           e.cat, e.text);
}

// Ring nach SD anhängen und leeren. NUR aus Core 1 (poll/flush), unter spiLock.
void writeOut() {
  if (!s_ring || !board::sdReady()) return;

  // Snapshot unter Mutex, danach SD-I/O ohne den Ring-Mutex zu halten.
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  int n = s_count;
  Entry* snap = nullptr;
  if (n > 0) {
    snap = (Entry*)malloc(sizeof(Entry) * n);
    if (snap) memcpy(snap, s_ring, sizeof(Entry) * n);
    else n = 0;
  }
  s_count = 0;   // Ring geleert (Append-Modus auf SD)
  xSemaphoreGive(s_mutex);
  if (n == 0) return;

  spiLock();
  SD.mkdir("/.fennek");
  // Rotation: zu groß → in .old umbenennen, frisch weiterschreiben.
  File chk = SD.open(kFile, FILE_READ);
  uint32_t sz = chk ? chk.size() : 0;
  if (chk) chk.close();
  if (sz > kMaxFileSize) {
    SD.remove(kFileOld);
    SD.rename(kFile, kFileOld);
  }
  File f = SD.open(kFile, FILE_APPEND);
  if (f) {
    char line[176];
    for (int i = 0; i < n; i++) {
      formatLine(snap[i], line, sizeof(line));
      f.print(line);
    }
    f.close();
  }
  spiUnlock();
  free(snap);
  Serial.printf("[BATTLOG] %d Zeilen nach %s\n", n, kFile);
}

}  // namespace

namespace battlog {

void begin() {
  s_mutex = xSemaphoreCreateMutex();
  s_ring  = (Entry*)heap_caps_malloc(sizeof(Entry) * kRingMax, MALLOC_CAP_SPIRAM);
  if (!s_ring) s_ring = (Entry*)malloc(sizeof(Entry) * kRingMax);
  s_count = 0;
  // Erst-Probe, damit schon die Boot-Zeile einen echten Akkustand trägt.
  s_cMv  = battery::milliVolts();
  s_cPct = battery::percent();
  s_cChg = battery::charging();
  s_haveCharge   = true;
  s_lastSampleMs = s_lastFlushMs = millis();
  append("Boot", "Fennek " FENNEK_VERSION);
  Serial.printf("[BATTLOG] aktiv (Debug) -> %s\n", kFile);
}

void event(const char* cat, const char* fmt, ...) {
  if (!s_ring) return;
  char buf[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  append(cat, buf);
}

void poll() {
  if (!s_ring) return;
  uint32_t now = millis();

  // Periodische Akku-Probe (liest I2C — nur hier, auf Core 1).
  if (now - s_lastSampleMs >= kSampleMs) {
    s_lastSampleMs = now;
    s_cMv  = battery::milliVolts();
    s_cPct = battery::percent();
    bool chg  = battery::charging();
    bool prev = s_cChg;
    s_cChg = chg;
    if (s_haveCharge && chg != prev) append("Laden", chg ? "gestartet" : "beendet");
    s_haveCharge = true;
    append("Akku", "Probe");
  }

  // Gedrosselt nach SD schreiben (oder wenn der Ring zu voll wird).
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  int cnt = s_count;
  xSemaphoreGive(s_mutex);
  if (cnt >= kFlushAt || (cnt > 0 && now - s_lastFlushMs >= kFlushMs)) {
    s_lastFlushMs = now;
    writeOut();
  }
}

void flush(const char* reason) {
  if (!s_ring) return;
  if (reason && reason[0]) append("Flush", reason);
  s_lastFlushMs = millis();
  writeOut();
}

}  // namespace battlog

#endif  // BATTLOG
