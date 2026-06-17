// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "gps.h"
#include "config.h"
#include "core/board.h"
#include "core/settings.h"
#include "services/timesync.h"

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

namespace gps {

namespace {

Fix      s_fix = {};
uint32_t s_lastMs = 0;

constexpr int kLineMax = 100;     // NMEA-Sätze sind <= 82 Zeichen
char  s_line[kLineMax];
int   s_len = 0;

// --- NMEA-Helfer ----------------------------------------------------------------

// Hex-Ziffer -> Wert (-1 = ungültig).
int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Prüft "$...*hh" und liefert den Zeiger auf das erste Feld (hinter dem '$')
// in einen Arbeitspuffer kopiert, mit '\0' an Stelle des '*'. nullptr = ungültig.
bool checksumOk(const char* line) {
  if (line[0] != '$') return false;
  uint8_t sum = 0;
  const char* p = line + 1;
  while (*p && *p != '*') { sum ^= (uint8_t)*p; p++; }
  if (*p != '*') return false;                 // keine Checksumme angehängt
  int hi = hexVal(p[1]), lo = hexVal(p[2]);
  if (hi < 0 || lo < 0) return false;
  return sum == (uint8_t)((hi << 4) | lo);
}

// NMEA-Breite/-Länge "ddmm.mmmm" + Hemisphäre -> Dezimalgrad. 0 bei leer.
double parseLatLon(const char* field, char hemi, int degDigits) {
  if (!field || !*field) return 0.0;
  double v = atof(field);                      // ddmm.mmmm bzw. dddmm.mmmm
  int deg = (int)(v / 100.0);
  double min = v - deg * 100.0;
  double dec = deg + min / 60.0;
  (void)degDigits;
  if (hemi == 'S' || hemi == 'W') dec = -dec;
  return dec;
}

// Tage seit 1970-01-01 für ein UTC-Datum (Howard Hinnant, proleptisch).
long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

// RMC liefert UTC als hhmmss(.ss) + Datum ddmmyy -> Unix-Sekunden (0 = leer).
uint32_t parseEpoch(const char* timeF, const char* dateF) {
  if (!timeF || !*timeF || !dateF || strlen(dateF) < 6) return 0;
  int hh = (timeF[0]-'0')*10 + (timeF[1]-'0');
  int mm = (timeF[2]-'0')*10 + (timeF[3]-'0');
  int ss = (timeF[4]-'0')*10 + (timeF[5]-'0');
  int dd = (dateF[0]-'0')*10 + (dateF[1]-'0');
  int mo = (dateF[2]-'0')*10 + (dateF[3]-'0');
  int yy = (dateF[4]-'0')*10 + (dateF[5]-'0');
  long days = daysFromCivil(2000 + yy, mo, dd);
  return (uint32_t)(days * 86400L + hh * 3600 + mm * 60 + ss);
}

// Zeile an ',' in bis zu maxF Felder zerlegen (Zeiger in buf, der zerstückelt
// wird). Liefert die Feldzahl.
int splitFields(char* buf, char** fields, int maxF) {
  int n = 0;
  char* p = buf;
  fields[n++] = p;
  while (*p && n < maxF) {
    if (*p == ',') { *p = '\0'; fields[n++] = p + 1; }
    else if (*p == '*') { *p = '\0'; break; }   // Checksumme abschneiden
    p++;
  }
  return n;
}

// --- UBX-Hilfsdaten-Injektion (schnellerer Fix) ---------------------------------
// u-blox M10 (MIA-M10Q): bekannte Uhrzeit + grobe Position als UBX-MGA-INI senden,
// damit der Empfänger gezielt nach sichtbaren Satelliten sucht statt blind kalt zu
// starten. Best-effort — kennt das Modul die Nachricht nicht, ignoriert es sie.
void sendUbx(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  uint8_t hdr[6] = {0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
  uint8_t ckA = 0, ckB = 0;
  for (int i = 2; i < 6; i++) { ckA += hdr[i]; ckB += ckA; }
  for (uint16_t i = 0; i < len; i++) { ckA += payload[i]; ckB += ckA; }
  Serial1.write(hdr, 6);
  if (len) Serial1.write(payload, len);
  uint8_t ck[2] = {ckA, ckB};
  Serial1.write(ck, 2);
}

// UBX-MGA-INI-TIME_UTC (0x13 0x40, type 0x10): aktuelle UTC injizieren.
void injectTime(uint32_t epoch) {
  time_t t = (time_t)epoch;
  struct tm g;
  gmtime_r(&t, &g);
  uint16_t year = (uint16_t)(g.tm_year + 1900);
  uint8_t p[24] = {0};
  p[0] = 0x10;          // type = UTC
  p[1] = 0x00;          // version
  p[2] = 0x00;          // ref (keine Quelle)
  p[3] = 0x80;          // leapSecs = -128 (unbekannt)
  p[4] = (uint8_t)(year & 0xFF); p[5] = (uint8_t)(year >> 8);
  p[6] = (uint8_t)(g.tm_mon + 1); p[7] = (uint8_t)g.tm_mday;
  p[8] = (uint8_t)g.tm_hour; p[9] = (uint8_t)g.tm_min; p[10] = (uint8_t)g.tm_sec;
  p[16] = 2;            // tAccS = 2 s (Genauigkeit der injizierten Zeit)
  sendUbx(0x13, 0x40, p, sizeof(p));
}

// UBX-MGA-INI-POS_LLH (0x13 0x40, type 0x01): grobe Position injizieren.
void injectPos(double lat, double lon) {
  int32_t la = (int32_t)lround(lat * 1e7);
  int32_t lo = (int32_t)lround(lon * 1e7);
  uint32_t acc = 50000UL * 100UL;   // 50 km in cm (letzte bekannte Position grob)
  uint8_t p[20] = {0};
  p[0] = 0x01;          // type = POS_LLH
  p[4]  = (uint8_t)(la);       p[5]  = (uint8_t)(la >> 8);
  p[6]  = (uint8_t)(la >> 16); p[7]  = (uint8_t)(la >> 24);
  p[8]  = (uint8_t)(lo);       p[9]  = (uint8_t)(lo >> 8);
  p[10] = (uint8_t)(lo >> 16); p[11] = (uint8_t)(lo >> 24);
  p[16] = (uint8_t)(acc);       p[17] = (uint8_t)(acc >> 8);
  p[18] = (uint8_t)(acc >> 16); p[19] = (uint8_t)(acc >> 24);
  sendUbx(0x13, 0x40, p, sizeof(p));
}

}  // namespace

bool parseLine(const char* line, Fix& fix) {
  if (!line || line[0] != '$') return false;
  if (!checksumOk(line)) return false;

  // Talker-ID ignorieren (GP/GN/GL/...): Satztyp steht an Position 3..5.
  const char* type = line + 3;

  char buf[kLineMax];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  char* f[20];
  int nf = splitFields(buf, f, 20);

  if (strncmp(type, "GGA", 3) == 0) {
    // $..GGA,time,lat,N/S,lon,E/W,qual,sats,hdop,alt,M,...
    if (nf < 10) return false;
    fix.quality = (uint8_t)atoi(f[6]);
    fix.sats    = (uint8_t)atoi(f[7]);
    fix.hdop    = (float)atof(f[8]);
    fix.altM    = (float)atof(f[9]);
    if (fix.quality > 0 && *f[2] && *f[4]) {
      fix.lat = parseLatLon(f[2], f[3][0], 2);
      fix.lon = parseLatLon(f[4], f[5][0], 3);
      fix.valid = true;
    } else if (fix.quality == 0) {
      fix.valid = false;
    }
    return true;
  }

  if (strncmp(type, "RMC", 3) == 0) {
    // $..RMC,time,status,lat,N/S,lon,E/W,speedKn,course,date,...
    if (nf < 10) return false;
    bool active = (f[2][0] == 'A');
    fix.valid = active;
    if (active && *f[3] && *f[5]) {
      fix.lat = parseLatLon(f[3], f[4][0], 2);
      fix.lon = parseLatLon(f[5], f[6][0], 3);
      fix.speedKmh = (float)atof(f[7]) * 1.852f;   // Knoten -> km/h
    }
    // Zeit übernehmen, sobald Datum + Zeit da sind — auch ohne Positionsfix, damit
    // die Uhr früher als die Position steht (Uhr-Sync, höchste Priorität).
    if (*f[1] && strlen(f[9]) >= 6) fix.epochUtc = parseEpoch(f[1], f[9]);
    return true;
  }

  return false;   // anderer Satz (GSV/GSA/VTG/...) — ignoriert, aber gültig
}

void begin() {
  board::gpsPower(true);
  s_fix = Fix{};
  s_len = 0;
  s_lastMs = 0;
  // Hilfsdaten injizieren (schnellerer Fix): bekannte Uhrzeit + letzte Position.
  delay(150);                       // Modul-Boot abwarten, dann UBX akzeptiert
  uint32_t t = timesync::now();
  if (t > 1700000000UL) injectTime(t);
  double lat = 0, lon = 0;
  settings::meshPos(&lat, &lon);
  if (lat != 0.0 || lon != 0.0) injectPos(lat, lon);
}

void end() {
  board::gpsPower(false);
}

void poll() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n' || c == '\r') {
      if (s_len > 0) {
        s_line[s_len] = '\0';
        if (parseLine(s_line, s_fix)) s_lastMs = millis();
        s_len = 0;
      }
    } else if (s_len < kLineMax - 1) {
      s_line[s_len++] = c;
    } else {
      s_len = 0;   // Überlauf (Müll/falsche Baudrate) — Zeile verwerfen
    }
  }
}

const Fix& current()      { return s_fix; }
uint32_t   lastSentenceMs() { return s_lastMs; }

}  // namespace gps
