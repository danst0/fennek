// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "console.h"
#include "config.h"
#include "core/battery.h"
#include "core/board.h"
#include "core/power.h"
#include "core/settings.h"
#include "apps/mesh_client.h"
#include "apps/reader_app.h"
#include "apps/notes_app.h"
#include "services/webfm.h"
#include "services/timesync.h"
#include "services/scrobble.h"
#include "services/alarmclock.h"
#include "services/settingsfile.h"

#include <time.h>

#include <Arduino.h>
#include <SD.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <string.h>
#include <stdlib.h>

namespace {

char s_line[200];
int  s_len = 0;

// Mesh bei Bedarf initialisieren (für Sende-Befehle ohne UI).
bool ensureMesh() {
  if (mesh_client::ready()) return true;
  Serial.println("[CON] Mesh nicht initialisiert — starte ...");
  bool ok = mesh_client::begin();
  Serial.printf("[CON] mesh init: %s\n", ok ? "OK" : "FEHLGESCHLAGEN");
  return ok;
}

void cmdHelp() {
  Serial.println("[CON] Befehle:");
  Serial.println("[CON]   status            - Systen-/Mesh-Status");
  Serial.println("[CON]   time              - Uhrzeit/Qualität/Quelle (Zeit-Sync)");
  Serial.println("[CON]   time set <...>    - Zeit setzen (YYYY-MM-DD HH:MM, lokal)");
  Serial.println("[CON]   time sync         - NTP-Sync jetzt erzwingen (WLAN)");
  Serial.println("[CON]   tz <POSIX-TZ>     - Zeitzone setzen (Default Europe/Berlin)");
  Serial.println("[CON]   sleep             - Standby ausloesen (wie Langdruck)");
  Serial.println("[CON]   alarm             - Wecker auflisten");
  Serial.println("[CON]   alarm <i> <hh:mm> [tage] - Wecker setzen (tage: taeglich|mo,di,..)");
  Serial.println("[CON]   alarm <i> off / <i> mode ton|blink|beides / sound <pfad> / test / stop / snooze");
  Serial.println("[CON]   mesh init         - Mesh-Radio initialisieren");
  Serial.println("[CON]   advert            - Zero-Hop-Advert senden (mit Akku-Telemetrie)");
  Serial.println("[CON]   pos [<lat> <lon>] - Node-Position zeigen/setzen (Standortbake im Advert)");
  Serial.println("[CON]   public <text>     - Nachricht an Public-Channel");
  Serial.println("[CON]   dm <idx> <text>   - Direktnachricht an Kontakt #idx");
  Serial.println("[CON]   contacts          - Kontaktliste");
  Serial.println("[CON]   contacts reset    - alle Kontakte löschen (Neuaufbau aus Adverts)");
  Serial.println("[CON]   channels          - beigetretene Kanäle");
  Serial.println("[CON]   join <name>       - Hashtag-Kanal beitreten (z.B. join test)");
  Serial.println("[CON]   chan <name> <txt> - Nachricht an Hashtag-Kanal");
  Serial.println("[CON]   msgs              - Nachrichten-Verlauf dumpen");
  Serial.println("[CON]   meshlog           - Ende des SD-Nachrichten-Logs zeigen");
  Serial.println("[CON]   battlog           - Akku-/Aktivitaets-Log (BATTLOG) dumpen");
  Serial.println("[CON]   i2cscan           - I2C-Bus abklopfen (RTC auf 0x51?)");
  Serial.println("[CON]   ls <pfad>         - SD-Verzeichnis listen (z.B. ls /books)");
  Serial.println("[CON]   rm <pfad>         - Datei/Ordner rekursiv loeschen (auch ueberlange Calibre-Pfade)");
  Serial.println("[CON]   books             - Bücher-Scan neu ausführen + dumpen");
  Serial.println("[CON]   wifi ssid <name>  - WLAN-SSID setzen (NVS)");
  Serial.println("[CON]   wifi pass <pw>    - WLAN-Passwort setzen (NVS)");
  Serial.println("[CON]   wifi status       - WLAN-/Webserver-Status");
  Serial.println("[CON]   wifi start        - Web-Dateiverwaltung starten");
  Serial.println("[CON]   wifi stop         - Web-Dateiverwaltung stoppen");
  Serial.println("[CON]   nav url <url>     - Navidrome-Server-URL setzen");
  Serial.println("[CON]   nav user <name>   - Navidrome-Benutzer setzen");
  Serial.println("[CON]   nav pass <pw>     - Navidrome-Passwort setzen");
  Serial.println("[CON]   nav on|off        - Scrobbeln ein-/ausschalten");
  Serial.println("[CON]   nav test          - Subsonic-Ping (WLAN)");
  Serial.println("[CON]   scrobble          - offene Scrobbles zeigen");
  Serial.println("[CON]   scrobble flush    - offene Scrobbles jetzt hochladen (WLAN)");
  Serial.println("[CON]   settings show     - aktuelle Einstellungen anzeigen");
  Serial.println("[CON]   settings save     - Einstellungen nach /fennek.ini (SD)");
  Serial.println("[CON]   settings load     - /fennek.ini ins NVS einlesen");
}

void cmdSettingsShow() {
  // exportIni() exportiert das WLAN-Passwort bewusst nicht (Feld bleibt leer),
  // daher gibt es hier nichts zu maskieren.
  static char buf[2048];
  settings::exportIni(buf, sizeof(buf));
  Serial.print(buf);
}

void cmdWifiStatus() {
  char ssid[33];
  webfm::ssid(ssid, sizeof(ssid));
  char pass[65];
  settings::wifiPass(pass, sizeof(pass));
  const char* st = "aus";
  switch (webfm::state()) {
    case webfm::State::CONNECTING: st = "verbinde ..."; break;
    case webfm::State::RUNNING:    st = "läuft"; break;
    case webfm::State::FAILED:     st = "FEHLGESCHLAGEN"; break;
    default: break;
  }
  char ip[16];
  webfm::ipStr(ip, sizeof(ip));
  Serial.printf("[CON] WiFi: SSID='%s' Passwort=%s | Status: %s%s%s | Requests=%lu\n",
                ssid[0] ? ssid : "(nicht gesetzt)",
                pass[0] ? "gesetzt" : "(nicht gesetzt)", st,
                ip[0] ? " http://" : "", ip,
                (unsigned long)webfm::requestCount());
}

// SD-Verzeichnis listen (Diagnose, nicht rekursiv).
void cmdLs(const char* path) {
  if (!board::sdReady()) { Serial.println("[CON] Keine SD-Karte"); return; }
  spiLock();
  File d = SD.open(path);
  if (!d) {
    spiUnlock();
    Serial.printf("[CON] '%s' existiert nicht\n", path);
    return;
  }
  if (!d.isDirectory()) {
    Serial.printf("[CON] %s ist eine Datei (%u Bytes)\n", path, (unsigned)d.size());
    d.close();
    spiUnlock();
    return;
  }
  int n = 0;
  File f;
  while ((f = d.openNextFile())) {
    if (f.isDirectory()) Serial.printf("[CON]   %s/\n", f.name());
    else                 Serial.printf("[CON]   %s  (%u Bytes)\n", f.name(), (unsigned)f.size());
    f.close();
    n++;
  }
  d.close();
  spiUnlock();
  Serial.printf("[CON] %d Eintrag/Einträge in %s\n", n, path);
}

// Rekursives Löschen für die Konsole. Eigener großer Pfadpuffer (512 statt der
// 192er-WebFM-Puffer), weil Calibre-Dateinamen den Pfad auf >240 Zeichen treiben
// — die einzelnen Pfad-Komponenten bleiben aber unter dem FATFS-Limit (255), nur
// die festen Firmware-Puffer waren zu klein. f.name() liefert in arduino-esp32
// 2.0.x den Basisnamen, daher Pfad selbst zusammensetzen. Aufruf unter spiLock!
bool rmTree(const char* path) {
  File f = SD.open(path);
  if (!f) return false;
  if (!f.isDirectory()) { f.close(); return SD.remove(path); }
  bool ok = true;
  File c;
  while (ok && (c = f.openNextFile())) {
    char child[512];
    snprintf(child, sizeof(child), "%s/%s", path, c.name());
    bool isDir = c.isDirectory();
    c.close();
    ok = isDir ? rmTree(child) : SD.remove(child);
  }
  f.close();
  return ok && SD.rmdir(path);
}

void cmdRm(const char* path) {
  if (!board::sdReady()) { Serial.println("[CON] Keine SD-Karte"); return; }
  // Sicherheitsnetz: keine Wurzel-/Mountpunkt-Löschung.
  if (path[0] != '/' || strlen(path) < 2 || strcmp(path, "/sd") == 0) {
    Serial.println("[CON] Ungueltiger Pfad");
    return;
  }
  spiLock();
  bool exists = SD.exists(path);
  bool ok = exists && rmTree(path);
  spiUnlock();
  if (!exists)   Serial.printf("[CON] '%s' existiert nicht\n", path);
  else if (ok)   Serial.printf("[CON] Geloescht: %s\n", path);
  else           Serial.printf("[CON] Loeschen (teilweise) fehlgeschlagen: %s\n", path);
}

// Letzte ~2 KB des SD-Nachrichten-Logs ausgeben.
void cmdMeshLog() {
  if (!board::sdReady()) { Serial.println("[CON] Keine SD-Karte"); return; }
  spiLock();
  if (!SD.exists("/meshcore/messages.log")) {
    spiUnlock();
    Serial.println("[CON] /meshcore/messages.log existiert (noch) nicht");
    return;
  }
  File f = SD.open("/meshcore/messages.log");
  if (!f) { spiUnlock(); Serial.println("[CON] Log nicht lesbar"); return; }
  uint32_t sz = f.size();
  uint32_t from = (sz > 2048) ? sz - 2048 : 0;
  f.seek(from);
  static char buf[2049];
  int rd = f.read((uint8_t*)buf, 2048);
  f.close();
  spiUnlock();
  if (rd <= 0) { Serial.println("[CON] Log leer"); return; }
  buf[rd] = '\0';
  Serial.printf("[CON] /meshcore/messages.log (%u Bytes, letzte %d):\n", (unsigned)sz, rd);
  Serial.println(buf);
}

// Akku-/Aktivitäts-Log (BATTLOG) komplett über Serial ausgeben. Gechunkt unter
// spiLock, Netz-/Serial-I/O bleibt simpel (USB, kein SPI). Nur sinnvoll mit
// -D BATTLOG; ohne das Flag existiert die Datei nicht.
void cmdBatLog() {
  if (!board::sdReady()) { Serial.println("[CON] Keine SD-Karte"); return; }
  const char* path = "/.fennek/battery.log";
  spiLock();
  if (!SD.exists(path)) {
    spiUnlock();
    Serial.println("[CON] /.fennek/battery.log existiert (noch) nicht (BATTLOG aktiv?)");
    return;
  }
  File f = SD.open(path, FILE_READ);
  if (!f) { spiUnlock(); Serial.println("[CON] Log nicht lesbar"); return; }
  uint32_t sz = f.size();
  spiUnlock();
  Serial.printf("[CON] %s (%u Bytes):\n", path, (unsigned)sz);
  // In Häppchen lesen — SD-Read unter spiLock, Serial-Ausgabe danach.
  static char buf[1025];
  uint32_t off = 0;
  while (off < sz) {
    spiLock();
    f.seek(off);
    int rd = f.read((uint8_t*)buf, 1024);
    spiUnlock();
    if (rd <= 0) break;
    buf[rd] = '\0';
    Serial.print(buf);
    off += rd;
  }
  spiLock();
  f.close();
  spiUnlock();
  Serial.println("\n[CON] --- battlog Ende ---");
}

// Bekannte Geräte auf dem geteilten I2C-Bus (SDA 13 / SCL 14) benennen, damit
// der Scan auf einen Blick verrät, ob ein Hardware-RTC bestückt ist.
const char* i2cKnownName(uint8_t addr) {
  switch (addr) {
    case 0x1A: return "CST328 Touch";
    case 0x28: return "BHI260AP Gyro/IMU";
    case 0x34: return "TCA8418 Tastatur";
    case 0x51: return "PCF85063/PCF8563 RTC?";   // genau das wollen wir wissen
    case 0x55: return "BQ27220 Fuel-Gauge";
    case 0x5A: return "DRV2605 Vibrationsmotor";
    case 0x6B: return "BQ25896 Ladekontroller";
    default:   return nullptr;
  }
}

// I2C-Bus abklopfen (0x08..0x77). Diagnose-Befehl: zeigt, was wirklich auf dem
// Bus hängt — insbesondere ob auf 0x51 ein RTC-Chip ackt (es gibt keinen; die
// Uhr läuft auf der ESP32-Systemzeit, frisch gehalten von services/timesync aus
// Mesh-Adverts + NTP). Läuft im Arduino-loop() wie der Rest des I2C-Pollings,
// daher kein SPI-Lock nötig (eigener Bus).
void cmdI2cScan() {
  Serial.println("[CON] I2C-Scan (SDA 13 / SCL 14):");
  int found = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      const char* name = i2cKnownName(addr);
      Serial.printf("[CON]   0x%02X  %s\n", addr, name ? name : "(unbekannt)");
      found++;
    }
  }
  Serial.printf("[CON] %d Gerät(e) gefunden. %s\n", found,
                found ? "" : "Bus stumm — Verkabelung/Pins prüfen.");
}

void cmdStatus() {
  Serial.printf("[CON] Heap=%uKB PSRAM=%uKB Akku=%umV/%u%%%s SD=%s\n",
                (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(ESP.getFreePsram() / 1024),
                battery::milliVolts(), battery::percent(),
                battery::charging() ? "+" : "",
                board::sdReady() ? "ja" : "nein");
  // Standby-Diagnose: Warum lief dieser Boot an? (1=Poweron/Hardware-Reset
  // 5=DeepSleep-Wake 12=SW; Wake 3=EXT1/Knopf 4=Timer). Achtung: Der UNTERE
  // Seitenknopf ist der Hardware-Reset -> immer Reset-Grund=1, Firmware
  // sieht den Druck nie.
  Serial.printf("[CON] Boot: Reset-Grund=%d Wake-Quelle=%d ext1=0x%llx GPIO0=%d\n",
                (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause(),
                (unsigned long long)esp_sleep_get_ext1_wakeup_status(),
                (int)digitalRead(0));
  if (!mesh_client::ready()) {
    Serial.println("[CON] Mesh: nicht initialisiert ('mesh init')");
    return;
  }
  int nf; uint32_t rx, tx;
  mesh_client::radioStats(&nf, &rx, &tx);
  settings::MeshParams p = settings::meshParams();
  Serial.printf("[CON] Mesh: '%s' @ %.3f MHz SF%u BW%.1f CR4/%u %udBm | NoiseFloor=%d dBm RX=%lu TX=%lu\n",
                mesh_client::nodeName(), (double)p.freqMhz, p.sf, (double)p.bwKhz,
                p.cr, p.txDbm, nf, (unsigned long)rx, (unsigned long)tx);
  Serial.printf("[CON] Kontakte=%d Nachrichten=%d RTC=%lu Quelle=%s estErr=%lus\n",
                mesh_client::contactCount(), mesh_client::msgCount(),
                (unsigned long)mesh_client::rtcTime(),
                timesync::source(), (unsigned long)timesync::estErrSeconds());
}

// Uhr-Diagnose (Zeit-Koordinator): lokale + UTC-Zeit, Quelle, Qualität, Drift.
void cmdTime() {
  uint32_t e = timesync::now();
  time_t   tt = (time_t)e;
  struct tm utc, loc;
  gmtime_r(&tt, &utc);
  localtime_r(&tt, &loc);   // Zeitzone via timesync::applyTimezone gesetzt
  char tz[48];
  settings::tzString(tz, sizeof(tz));
  Serial.printf("[CON] Zeit lokal: %04d-%02d-%02d %02d:%02d:%02d (%s)\n",
                loc.tm_year + 1900, loc.tm_mon + 1, loc.tm_mday,
                loc.tm_hour, loc.tm_min, loc.tm_sec, tz);
  Serial.printf("[CON]   UTC=%lu (%02d:%02d:%02d) Quelle=%s Qualität=%s "
                "estErr=%lus Drift=%uppm\n",
                (unsigned long)e, utc.tm_hour, utc.tm_min, utc.tm_sec,
                timesync::source(), timesync::qualityStr(),
                (unsigned long)timesync::estErrSeconds(), timesync::driftPpm());
}

// `time set YYYY-MM-DD HH:MM[:SS]` — lokale Zeit setzen (über die Zeitzone nach
// UTC umgerechnet). Fallback, wenn nie WLAN/Mesh erreichbar ist.
void cmdTimeSet(const char* arg) {
  struct tm lt;
  memset(&lt, 0, sizeof(lt));
  int y, mo, d, h, mi, s = 0;
  int got = sscanf(arg, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s);
  if (got < 5) {
    Serial.println("[CON] Nutzung: time set YYYY-MM-DD HH:MM[:SS] (lokale Zeit)");
    return;
  }
  lt.tm_year = y - 1900; lt.tm_mon = mo - 1; lt.tm_mday = d;
  lt.tm_hour = h; lt.tm_min = mi; lt.tm_sec = s;
  lt.tm_isdst = -1;                  // mktime soll Sommerzeit selbst bestimmen
  time_t utc = mktime(&lt);          // interpretiert lt in der gesetzten Zeitzone
  if (utc <= 0) { Serial.println("[CON] Ungültiges Datum"); return; }
  timesync::setManualTime((uint32_t)utc);
  cmdTime();
}

void cmdContacts() {
  if (!mesh_client::ready()) { Serial.println("[CON] Mesh nicht initialisiert"); return; }
  int n = mesh_client::contactCount();
  Serial.printf("[CON] %d Kontakt(e):\n", n);
  for (int i = 0; i < n; i++) {
    char nm[40];
    mesh_client::contactName(i, nm, sizeof(nm));
    double lat, lon;
    if (mesh_client::contactPos(i, &lat, &lon))
      Serial.printf("[CON]   [%d] %s  @ %.5f,%.5f\n", i, nm, lat, lon);
    else
      Serial.printf("[CON]   [%d] %s\n", i, nm);
  }
}

// Node-Position zeigen (ohne Argument) oder setzen ("pos <lat> <lon>"). Die
// Position wird in NVS persistiert und mit dem nächsten Advert mitgesendet;
// funktioniert auch ohne initialisiertes Mesh (rein Settings-seitig).
void cmdPos(const char* arg) {
  if (!arg || !*arg) {
    double lat, lon;
    mesh_client::nodePos(&lat, &lon);
    if (lat == 0.0 && lon == 0.0) Serial.println("[CON] Keine Position gesetzt (pos <lat> <lon>)");
    else                          Serial.printf("[CON] Position: %.6f, %.6f\n", lat, lon);
    return;
  }
  const char* sp = strchr(arg, ' ');
  if (!sp) { Serial.println("[CON] Nutzung: pos <lat> <lon>"); return; }
  double lat = atof(arg);
  double lon = atof(sp + 1);
  mesh_client::setNodePosition(lat, lon);
  Serial.printf("[CON] Position gesetzt: %.6f, %.6f (advert sendet sie nun mit)\n", lat, lon);
}

void alarmPrintList() {
  Serial.println("[CON] Wecker:");
  const char* dn[7] = {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"};
  for (int i = 0; i < alarmclock::count(); i++) {
    alarmclock::Alarm a = alarmclock::get(i);
    if (!a.enabled) { Serial.printf("[CON]   [%d] --:--  (aus)\n", i); continue; }
    char days[40] = "";
    if (a.dowMask == 0) strcpy(days, "taeglich");
    else for (int d = 0; d < 7; d++)
      if (a.dowMask & (1 << d)) { strcat(days, dn[d]); strcat(days, " "); }
    const char* sig = a.signal == alarmclock::SIG_TONE ? "Ton"
                    : a.signal == alarmclock::SIG_BLINK ? "Blink" : "Ton+Blink";
    Serial.printf("[CON]   [%d] %02u:%02u  %-9s  %s\n", i, a.hour, a.minute, days, sig);
  }
  char snd[TRACK_PATH_LEN];
  alarmclock::soundPath(snd, sizeof(snd));
  Serial.printf("[CON]   Klingelton: %s\n", snd[0] ? snd : "(erster Titel)");
}

// "mo,di,fr" -> Bitmaske (bit0=Mo); "taeglich"/leer -> 0 (täglich).
uint8_t alarmParseDays(const char* s) {
  if (!s || !*s) return 0;
  if (strstr(s, "taegl") || strstr(s, "dai") || strstr(s, "all")) return 0;
  static const char* k[7] = {"mo", "di", "mi", "do", "fr", "sa", "so"};
  uint8_t m = 0;
  for (int i = 0; i < 7; i++) if (strstr(s, k[i])) m |= (1 << i);
  return m;
}

void cmdAlarm(const char* arg) {
  while (arg && *arg == ' ') arg++;
  if (!arg || !*arg)                  { alarmPrintList(); return; }
  if (strcmp(arg, "test") == 0)       { alarmclock::fireNow(); return; }
  if (strcmp(arg, "stop") == 0)       { alarmclock::dismiss(); return; }
  if (strcmp(arg, "snooze") == 0)     { alarmclock::snooze(); return; }
  if (strncmp(arg, "sound ", 6) == 0) {
    alarmclock::setSoundPath(arg + 6);
    Serial.printf("[CON] Klingelton: %s\n", arg + 6);
    return;
  }
  int i = atoi(arg);
  const char* sp = strchr(arg, ' ');
  if (i < 0 || i >= alarmclock::count() || !sp) {
    Serial.println("[CON] Nutzung: alarm <0..3> <hh:mm> [tage] | <i> off | <i> mode ton|blink|beides");
    return;
  }
  sp++;
  if (strncmp(sp, "off", 3) == 0) {
    alarmclock::clear(i);
    Serial.printf("[CON] Wecker %d aus\n", i);
    return;
  }
  if (strncmp(sp, "mode ", 5) == 0) {                  // Signal-Modus pro Wecker
    const char* m = sp + 5;
    uint8_t v = strstr(m, "ton") ? alarmclock::SIG_TONE
              : strstr(m, "blink") ? alarmclock::SIG_BLINK
              : (strstr(m, "beid") || strstr(m, "both")) ? alarmclock::SIG_BOTH : 0;
    if (!v) { Serial.println("[CON] alarm <i> mode ton|blink|beides"); return; }
    alarmclock::Alarm a = alarmclock::get(i);
    a.signal = v;
    alarmclock::set(i, a);
    Serial.printf("[CON] Wecker %d Signal: %u (1=Ton 2=Blink 3=beides)\n", i, v);
    return;
  }
  int hh, mm;
  if (sscanf(sp, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
    Serial.println("[CON] Zeit als hh:mm (z.B. 06:30)");
    return;
  }
  const char* daysArg = strchr(sp, ' ');
  alarmclock::Alarm a = alarmclock::get(i);            // Signal/Rest erhalten
  a.enabled = true;
  a.hour = (uint8_t)hh;
  a.minute = (uint8_t)mm;
  a.dowMask = alarmParseDays(daysArg ? daysArg + 1 : "");
  alarmclock::set(i, a);
  Serial.printf("[CON] Wecker %d: %02d:%02d %s gesetzt\n", i, hh, mm,
                a.dowMask ? "(Wochentage)" : "(taeglich)");
}

void cmdMsgs() {
  int n = mesh_client::msgCount();
  Serial.printf("[CON] %d Nachricht(en) im Verlauf:\n", n);
  for (int i = 0; i < n; i++) {
    mesh_client::MsgView m;
    if (!mesh_client::msg(i, m)) continue;
    const char* kind = (m.kind == 0) ? "CH " : (m.kind == 1) ? "DM<" : "DM>";
    const char* ack  = (m.kind != 2) ? ""
                     : (m.ackState == 2) ? " [zugestellt]"
                     : (m.ackState == 3) ? " [timeout]"
                     : (m.ackState == 1) ? " [ausstehend]" : "";
    Serial.printf("[CON]   %s %lu %s%s%s%s\n", kind, (unsigned long)m.timestamp,
                  m.from[0] ? m.from : "", m.from[0] ? ": " : "", m.text, ack);
  }
}

void handleLine(char* line) {
  // Führende Leerzeichen überspringen.
  while (*line == ' ') line++;
  if (!*line) return;

  // Konsolen-Befehle zählen als Aktivität — sonst schlägt der Auto-Standby
  // mitten in seriellen Mesh-Tests zu (passiert am 12.06.2026, zweimal).
  power::noteActivity();

  if (strcmp(line, "help") == 0)            { cmdHelp(); return; }
  if (strcmp(line, "status") == 0)          { cmdStatus(); return; }
  if (strcmp(line, "time") == 0)            { cmdTime(); return; }
  if (strncmp(line, "time set ", 9) == 0)   { cmdTimeSet(line + 9); return; }
  if (strcmp(line, "time sync") == 0) {
    if (!timesync::forceSyncNow()) Serial.println("[CON] NTP-Sync fehlgeschlagen");
    else cmdTime();
    return;
  }
  if (strncmp(line, "tz ", 3) == 0) {
    settings::setTzString(line + 3);
    timesync::applyTimezone();
    Serial.printf("[CON] Zeitzone gesetzt: '%s'\n", line + 3);
    cmdTime();
    return;
  }
  if (strcmp(line, "mesh init") == 0)       { ensureMesh(); return; }
  if (strcmp(line, "contacts reset") == 0)  {
    if (!ensureMesh()) return;
    mesh_client::resetContacts();
    Serial.println("[CON] Kontakte gelöscht (Neuaufbau aus Adverts)");
    return;
  }
  if (strcmp(line, "contacts") == 0)        { cmdContacts(); return; }
  if (strcmp(line, "msgs") == 0)            { cmdMsgs(); return; }
  if (strcmp(line, "meshlog") == 0)         { cmdMeshLog(); return; }
  if (strcmp(line, "battlog") == 0)         { cmdBatLog(); return; }
  if (strcmp(line, "i2cscan") == 0)         { cmdI2cScan(); return; }
  if (strcmp(line, "ls") == 0)              { cmdLs("/"); return; }
  if (strncmp(line, "ls ", 3) == 0)         { cmdLs(line + 3); return; }
  if (strncmp(line, "rm ", 3) == 0)         { cmdRm(line + 3); return; }
  if (strcmp(line, "books") == 0)           { reader_app::debugScan(); return; }
  if (strcmp(line, "notes") == 0)           { notes_app::debugSmoke(); return; }
  if (strncmp(line, "wifi ssid ", 10) == 0) {
    settings::setWifiSsid(line + 10);
    Serial.printf("[CON] WLAN-SSID gesetzt: '%s'\n", line + 10);
    return;
  }
  if (strncmp(line, "wifi pass ", 10) == 0) {
    settings::setWifiPass(line + 10);
    Serial.println("[CON] WLAN-Passwort gesetzt");
    return;
  }
  if (strcmp(line, "wifi status") == 0)     { cmdWifiStatus(); return; }
  if (strcmp(line, "sleep") == 0)           { power::enterStandby(); return; }
  if (strcmp(line, "wifi start") == 0) {
    if (!webfm::start()) Serial.println("[CON] Start fehlgeschlagen — erst 'wifi ssid <name>' setzen");
    return;
  }
  if (strcmp(line, "wifi stop") == 0)       { webfm::stop(); return; }
  if (strncmp(line, "nav url ", 8) == 0) {
    settings::setNavUrl(line + 8);
    Serial.printf("[CON] Navidrome-URL gesetzt: '%s'\n", line + 8);
    return;
  }
  if (strncmp(line, "nav user ", 9) == 0) {
    settings::setNavUser(line + 9);
    Serial.printf("[CON] Navidrome-Benutzer gesetzt: '%s'\n", line + 9);
    return;
  }
  if (strncmp(line, "nav pass ", 9) == 0) {
    settings::setNavPass(line + 9);
    Serial.println("[CON] Navidrome-Passwort gesetzt");
    return;
  }
  if (strcmp(line, "nav on") == 0)  { settings::setNavEnabled(true);  Serial.println("[CON] Scrobbeln EIN"); return; }
  if (strcmp(line, "nav off") == 0) { settings::setNavEnabled(false); Serial.println("[CON] Scrobbeln AUS"); return; }
  if (strcmp(line, "nav test") == 0) {
    char msg[64];
    bool ok = scrobble::ping(msg, sizeof(msg));
    Serial.printf("[CON] Navidrome-Test: %s (%s)\n", ok ? "OK" : "Fehler", msg);
    return;
  }
  if (strcmp(line, "scrobble") == 0) {
    Serial.printf("[CON] %d offene Scrobble(s); Scrobbeln=%s\n",
                  scrobble::pendingCount(), settings::navEnabled() ? "an" : "aus");
    return;
  }
  if (strcmp(line, "scrobble flush") == 0) {
    char msg[64];
    scrobble::flushNow(msg, sizeof(msg));
    Serial.printf("[CON] Scrobble-Flush: %s\n", msg);
    return;
  }
  if (strcmp(line, "settings show") == 0)   { cmdSettingsShow(); return; }
  if (strcmp(line, "settings save") == 0) {
    Serial.println(settingsfile::exportToSd()
        ? "[CON] Einstellungen nach /fennek.ini gesichert"
        : "[CON] Sichern fehlgeschlagen (keine SD?)");
    return;
  }
  if (strcmp(line, "settings load") == 0) {
    Serial.println(settingsfile::importFromSd()
        ? "[CON] Einstellungen aus /fennek.ini geladen"
        : "[CON] Laden fehlgeschlagen (keine SD / Datei fehlt?)");
    return;
  }
  if (strcmp(line, "channels") == 0) {
    if (!mesh_client::ready()) { Serial.println("[CON] Mesh nicht initialisiert"); return; }
    int n = mesh_client::channelCount();
    Serial.printf("[CON] %d Kanal/Kanäle:\n", n);
    for (int i = 0; i < n; i++) {
      char nm[36];
      mesh_client::channelName(i, nm, sizeof(nm));
      Serial.printf("[CON]   [%d] %s\n", i, nm);
    }
    return;
  }
  if (strncmp(line, "join ", 5) == 0) {
    if (ensureMesh()) {
      int idx = mesh_client::joinHashChannel(line + 5);
      Serial.printf("[CON] join: %s (Index %d)\n", idx >= 0 ? "OK" : "FEHLER", idx);
    }
    return;
  }
  if (strncmp(line, "chan ", 5) == 0) {
    if (ensureMesh()) {
      // "chan <name> <text>"
      char name[40];
      const char* p = line + 5;
      int i = 0;
      while (*p && *p != ' ' && i < (int)sizeof(name) - 1) name[i++] = *p++;
      name[i] = '\0';
      while (*p == ' ') p++;
      if (!name[0] || !*p) { Serial.println("[CON] Nutzung: chan <name> <text>"); return; }
      bool ok = mesh_client::sendHashChannelMsg(name, p);
      Serial.printf("[CON] chan #%s: %s\n", name, ok ? "gesendet (flood)" : "FEHLER");
    }
    return;
  }
  if (strcmp(line, "advert") == 0) {
    if (ensureMesh()) { mesh_client::sendAdvert(); Serial.println("[CON] Advert gesendet (zero hop)"); }
    return;
  }
  if (strcmp(line, "pos") == 0)             { cmdPos(nullptr); return; }
  if (strncmp(line, "pos ", 4) == 0)        { cmdPos(line + 4); return; }
  if (strcmp(line, "alarm") == 0)           { cmdAlarm(""); return; }
  if (strncmp(line, "alarm ", 6) == 0)      { cmdAlarm(line + 6); return; }
  if (strncmp(line, "public ", 7) == 0) {
    if (ensureMesh()) {
      bool ok = mesh_client::sendChannelMsg(line + 7);
      Serial.printf("[CON] public: %s\n", ok ? "gesendet (flood)" : "FEHLER");
    }
    return;
  }
  if (strncmp(line, "dm ", 3) == 0) {
    if (ensureMesh()) {
      char* end = nullptr;
      long idx = strtol(line + 3, &end, 10);
      while (end && *end == ' ') end++;
      if (!end || !*end) { Serial.println("[CON] Nutzung: dm <idx> <text>"); return; }
      bool ok = mesh_client::sendDirectMsg((int)idx, end);
      Serial.printf("[CON] dm an [%ld]: %s\n", idx, ok ? "gesendet" : "FEHLER");
    }
    return;
  }
  Serial.printf("[CON] Unbekannter Befehl: '%s' ('help' zeigt die Liste)\n", line);
}

}  // namespace

namespace console {

void poll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (s_len > 0) {
        s_line[s_len] = '\0';
        s_len = 0;
        handleLine(s_line);
      }
    } else if (s_len < (int)sizeof(s_line) - 1) {
      s_line[s_len++] = c;
    }
  }
}

}  // namespace console
