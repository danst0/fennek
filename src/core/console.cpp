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
#include "services/battlog.h"
#include "services/webfm.h"
#include "services/timesync.h"
#include "services/notes_ai.h"
#include "services/scrobble.h"
#include "services/podcast.h"
#include "services/calendar.h"
#include "services/calibre_books.h"
#include "services/reinschrift.h"
#include "services/ota.h"
#include "services/alarmclock.h"
#include "services/settingsfile.h"
#include "services/gps.h"
#include "services/gyro.h"
#include "services/mic.h"
#include "services/audio.h"

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
  Serial.println("[CON]   alarm <i> <hh:mm> [tage] - Wecker setzen (tage: einmal|taeglich|mo,di,..)");
  Serial.println("[CON]   alarm <i> off / <i> mode ton|blink|vibra|beides|alle / sound <pfad> / test / stop / snooze");
  Serial.println("[CON]   mesh init         - Mesh-Radio initialisieren");
  Serial.println("[CON]   mesh eco [on|off] - RX-Duty-Cycle (Radio-Sparmodus) zeigen/setzen");
  Serial.println("[CON]   advert            - Zero-Hop-Advert senden (mit Akku-Telemetrie)");
  Serial.println("[CON]   advert flood      - Flood-Advert (mehrhopfaehig, erreicht entfernte Nodes)");
  Serial.println("[CON]   pos [<lat> <lon>] - Node-Position zeigen/setzen (Standortbake im Advert)");
  Serial.println("[CON]   gps [sek] / off   - GPS-Test: rohe NMEA + Fix lesen (Default 10 s), Modul aus");
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
  Serial.println("[CON]   gauge             - BQ27220-Register dumpen (RM/FCC/SoC, read-only)");
  Serial.println("[CON]   i2cscan           - I2C-Bus abklopfen (RTC auf 0x51?)");
  Serial.println("[CON]   ls <pfad>         - SD-Verzeichnis listen (z.B. ls /books)");
  Serial.println("[CON]   rm <pfad>         - Datei/Ordner rekursiv loeschen (auch ueberlange Calibre-Pfade)");
  Serial.println("[CON]   books             - Bücher-Scan neu ausführen + dumpen");
  Serial.println("[CON]   wifi list         - bekannte WLANs auflisten");
  Serial.println("[CON]   wifi ssid <name>  - WLAN-SSID von Profil 0 setzen (NVS)");
  Serial.println("[CON]   wifi pass <pw>    - WLAN-Passwort von Profil 0 setzen (NVS)");
  Serial.println("[CON]   wifi add <name> [pw] - weiteres WLAN-Profil anlegen");
  Serial.println("[CON]   wifi del <n>      - WLAN-Profil n loeschen");
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
  Serial.println("[CON]   ollama url <url>  - Ollama-Server (z. B. http://10.0.0.5:11434)");
  Serial.println("[CON]   ollama model <m>  - Ollama-Modell (z. B. llama3.2)");
  Serial.println("[CON]   ollama on|off     - Notizen automatisch schoenschreiben");
  Serial.println("[CON]   ollama / test     - Status / Verbindungstest (WLAN)");
  Serial.println("[CON]   ollama flush      - offene Notizen jetzt polieren (WLAN)");
  Serial.println("[CON]   podcast           - Feeds + lokale Folgen zeigen");
  Serial.println("[CON]   podcast feed <url>- Feed abonnieren (feeds.txt)");
  Serial.println("[CON]   podcast rm <idx>  - Feed entfernen");
  Serial.println("[CON]   podcast on|off    - Auto-Sync vor Standby ein/aus");
  Serial.println("[CON]   podcast sync      - neueste Folgen jetzt laden (WLAN)");
  Serial.println("[CON]   calibre           - Calibre-Sync-Status");
  Serial.println("[CON]   calibre url <url> - Calibre-Web (z. B. https://calibre.example)");
  Serial.println("[CON]   calibre user/pass - Calibre-Web-Login (OPDS-Basic-Auth)");
  Serial.println("[CON]   calibre shelf <s> - Buecherregal, Name oder ID (Default Fennek)");
  Serial.println("[CON]   calibre on|off    - Auto-Sync vor Standby ein/aus");
  Serial.println("[CON]   calibre sync      - neue Buecher jetzt laden (WLAN)");
  Serial.println("[CON]   ota               - OTA-Status (Version + Update-URL)");
  Serial.println("[CON]   ota url <url>     - Update-Quelle (GitHub-Release-API/Manifest)");
  Serial.println("[CON]   ota check         - auf neue Firmware pruefen (WLAN)");
  Serial.println("[CON]   ota update|force  - Firmware laden + flashen + Reboot (WLAN)");
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
  Serial.printf("[CON] WiFi: SSID='%s' Passwort=%s | Profile: %d | Status: %s%s%s | Requests=%lu\n",
                ssid[0] ? ssid : "(nicht gesetzt)",
                pass[0] ? "gesetzt" : "(nicht gesetzt)", settings::wifiCount(), st,
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
  // Erst den PSRAM-Ring auf die SD spülen: sonst fehlen genau die Zeilen der
  // laufenden Sitzung (der Ring wird nur alle 2 min bzw. vor dem Standby
  // geschrieben) — und dazu gehören die „Schlaf"-Zeilen aus logSleepDebug(),
  // also der Grund, warum man das Log überhaupt aufmacht. Da das Öffnen des
  // USB-Ports das Gerät resettet, wäre der Ring beim nächsten Dump ohnehin weg.
  BATTLOG_FLUSH("Konsole");
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

// Beliebige SD-Textdatei gechunkt über Serial ausgeben (SD-Read unter spiLock,
// Serial danach) — z. B. `cat /.fennek/battery.log`.
void cmdCat(const char* path) {
  if (!board::sdReady()) { Serial.println("[CON] Keine SD-Karte"); return; }
  spiLock();
  bool ok = SD.exists(path);
  File f = ok ? SD.open(path, FILE_READ) : File();
  uint32_t sz = f ? f.size() : 0;
  spiUnlock();
  if (!ok || !f) { Serial.printf("[CON] %s nicht lesbar\n", path); return; }
  Serial.printf("[CON] %s (%u Bytes):\n", path, (unsigned)sz);
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
  Serial.println("\n[CON] --- cat Ende ---");
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
  // Hinweis: Die Konsole selbst hält während des Befehls den 240-MHz-Boost —
  // der Wert zeigt also den Boost-Takt, nicht den Leerlauf-Basistakt (80).
  Serial.printf("[CON] Heap=%uKB PSRAM=%uKB CPU=%luMHz Akku=%umV/%u%%%s SD=%s\n",
                (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(ESP.getFreePsram() / 1024),
                (unsigned long)getCpuFrequencyMhz(),
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
    if (a.once) strcpy(days, "einmalig");
    else if (a.dowMask == 0) strcpy(days, "taeglich");
    else for (int d = 0; d < 7; d++)
      if (a.dowMask & (1 << d)) { strcat(days, dn[d]); strcat(days, " "); }
    char sig[24] = "";
    if (a.signal & alarmclock::SIG_TONE)  strcat(sig, "Ton ");
    if (a.signal & alarmclock::SIG_BLINK) strcat(sig, "Blink ");
    if (a.signal & alarmclock::SIG_VIBRA) strcat(sig, "Vibra ");
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
    uint8_t v = strstr(m, "vibr") ? alarmclock::SIG_VIBRA
              : strstr(m, "alle") ? (uint8_t)(alarmclock::SIG_TONE | alarmclock::SIG_BLINK | alarmclock::SIG_VIBRA)
              : strstr(m, "ton") ? alarmclock::SIG_TONE
              : strstr(m, "blink") ? alarmclock::SIG_BLINK
              : (strstr(m, "beid") || strstr(m, "both")) ? alarmclock::SIG_BOTH : 0;
    if (!v) { Serial.println("[CON] alarm <i> mode ton|blink|vibra|beides|alle"); return; }
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
  const char* daysStr = daysArg ? daysArg + 1 : "";
  alarmclock::Alarm a = alarmclock::get(i);            // Signal/Rest erhalten
  a.enabled = true;
  a.hour = (uint8_t)hh;
  a.minute = (uint8_t)mm;
  a.once = strstr(daysStr, "einmal") || strstr(daysStr, "once") || strstr(daysStr, "1x");
  a.dowMask = a.once ? 0 : alarmParseDays(daysStr);
  alarmclock::set(i, a);
  Serial.printf("[CON] Wecker %d: %02d:%02d %s gesetzt\n", i, hh, mm,
                a.once ? "(einmalig)" : a.dowMask ? "(Wochentage)" : "(taeglich)");
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

// GPS headless testen: Modul an, N s rohe NMEA + dekodierten Fix loggen, aus.
// Bestätigt Verkabelung/Enable/Baudrate ohne Display (wie Mesh-/Wecker-Tests).
void cmdGps(const char* arg) {
  if (arg && strncmp(arg, "off", 3) == 0) {
    gps::end();
    Serial.println("[GPS] Modul aus");
    return;
  }
  if (arg && strncmp(arg, "scan", 4) == 0) {
    // Gängige GPS-Baudraten durchprobieren und gültige NMEA-Zeilen zählen.
    pinMode(PIN_GPS_EN, OUTPUT);
    digitalWrite(PIN_GPS_EN, HIGH);
    delay(50);
    const long bauds[] = {9600, 38400, 115200, 57600, 4800};
    Serial.println("[GPS] Baudraten-Scan (je 3 s) ...");
    for (long b : bauds) {
      Serial1.begin(b, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
      delay(80);
      while (Serial1.available()) Serial1.read();   // Puffer leeren
      char line[100]; int len = 0;
      unsigned long bytes = 0, good = 0;
      char sample[100]; sample[0] = '\0';
      uint32_t t0 = millis();
      while (millis() - t0 < 3000) {
        while (Serial1.available()) {
          char c = (char)Serial1.read();
          bytes++;
          if (c == '\n' || c == '\r') {
            if (len > 0) {
              line[len] = '\0';
              if (line[0] == '$' && strchr(line, '*')) {
                good++;
                if (!sample[0]) strncpy(sample, line, sizeof(sample) - 1);
              }
              len = 0;
            }
          } else if (len < (int)sizeof(line) - 1) line[len++] = c;
          else len = 0;
        }
        power::noteActivity();
        delay(2);
      }
      Serial1.end();
      Serial.printf("[GPS]   %6ld Baud: %lu Bytes, %lu NMEA-Zeilen%s%s\n",
                    b, bytes, good, sample[0] ? "  z.B. " : "", sample);
    }
    digitalWrite(PIN_GPS_EN, LOW);
    Serial.println("[GPS] Scan fertig — Baud mit NMEA-Zeilen = GPS_BAUD setzen.");
    return;
  }
  int secs = 10;
  if (arg && *arg) { int v = atoi(arg); if (v > 0 && v <= 120) secs = v; }
  Serial.printf("[GPS] Modul an (Baud %d), lese %d s rohe NMEA ...\n", GPS_BAUD, secs);
  gps::begin();

  gps::Fix fix = {};
  char line[100];
  int  len = 0;
  uint32_t t0 = millis();
  unsigned long bytes = 0, lines = 0;
  while (millis() - t0 < (uint32_t)secs * 1000) {
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      bytes++;
      if (c == '\n' || c == '\r') {
        if (len > 0) {
          line[len] = '\0'; len = 0; lines++;
          Serial.printf("[GPS] %s\n", line);
          gps::parseLine(line, fix);
        }
      } else if (len < (int)sizeof(line) - 1) {
        line[len++] = c;
      } else {
        len = 0;   // Überlauf (Müll) — Zeile verwerfen
      }
    }
    power::noteActivity();   // Auto-Standby während des Tests verhindern
    delay(5);
  }

  Serial.printf("[GPS] %lu Bytes, %lu Zeilen empfangen\n", bytes, lines);
  if (bytes == 0)
    Serial.println("[GPS] KEINE Daten — Verkabelung/Enable (GPIO39) prüfen.");
  else if (lines == 0)
    Serial.println("[GPS] Bytes, aber keine gültigen Zeilen — vermutlich falsche Baudrate (GPS_BAUD).");
  // Zeit kommt oft vor dem Positionsfix (RMC-Datum+Zeit) — als Uhr-Sync melden.
  if (fix.epochUtc > 1700000000UL) {
    timesync::gpsSync(fix.epochUtc);
    Serial.printf("[GPS] UTC=%lu an timesync gemeldet (Quelle jetzt '%s')\n",
                  (unsigned long)fix.epochUtc, timesync::source());
  }
  if (fix.valid)
    Serial.printf("[GPS] FIX: %.6f, %.6f  Sat=%u HDOP=%.1f Alt=%.0fm v=%.1fkm/h\n",
                  fix.lat, fix.lon, (unsigned)fix.sats, (double)fix.hdop,
                  (double)fix.altM, (double)fix.speedKmh);
  else
    Serial.printf("[GPS] noch kein Positionsfix (zuletzt Sat=%u) — unter freiem Himmel erneut testen.\n",
                  (unsigned)fix.sats);
  gps::end();
  Serial.println("[GPS] Modul aus");
}

// BHI260-IMU testen: Firmware laden (~10 s), 2 s lesen, Werte ausgeben.
void cmdGyro() {
  Serial.println("[CON] Gyro: BHI260 init (Firmware-Upload ~10 s) ...");
  if (!gyro::begin()) { Serial.println("[CON] Gyro init fehlgeschlagen"); return; }
  for (int i = 0; i < 20; i++) {
    gyro::poll();
    delay(100);
    if (i % 5 == 4) {
      const gyro::Data& d = gyro::current();
      Serial.printf("[CON] acc(g) %+.2f %+.2f %+.2f | gyro(dps) %+.1f %+.1f %+.1f\n",
                    d.ax, d.ay, d.az, d.gx, d.gy, d.gz);
    }
  }
  gyro::end();
  Serial.println("[CON] Gyro: fertig (Sensor auf 0 Hz)");
}

// Mikro-Aufnahmetest: N s nach /rectest.wav aufnehmen (Standard 3 s).
void cmdRec(const char* arg) {
  int secs = 3;
  if (arg && *arg) { int v = atoi(arg); if (v > 0 && v <= 30) secs = v; }
  const char* path = "/rectest.wav";
  Serial.printf("[CON] Aufnahme %d s -> %s (jetzt sprechen!) ...\n", secs, path);
  if (!audio::beginMic()) { Serial.println("[CON] I2S0-Handover fehlgeschlagen"); return; }
  if (!mic::startRecording(path)) { audio::endMic(); Serial.println("[CON] Mikro-Init/SD fehlgeschlagen"); return; }
  uint16_t maxLvl = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < (uint32_t)secs * 1000) {
    mic::poll();
    uint16_t l = mic::level(); if (l > maxLvl) maxLvl = l;
    delay(5);
  }
  uint32_t dur = mic::stopRecording();
  audio::endMic();   // I2S0 zurück an die Audio-Engine
  Serial.printf("[CON] Aufnahme fertig: %lu s, Spitzenpegel %u/32767 %s\n",
                (unsigned long)dur, maxLvl,
                maxLvl > 1500 ? "(Ton erkannt)" : "(sehr leise/Stille?)");
  Serial.println("[CON] Abspielen: via WebFM herunterladen oder in /music kopieren.");
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
  if (strncmp(line, "mesh eco", 8) == 0) {
    const char* a = line + 8;
    while (*a == ' ') a++;
    if (*a) {
      settings::setMeshEco(strcmp(a, "on") == 0 || strcmp(a, "1") == 0);
      // Läuft das Radio schon, sofort umschalten (armiert den Empfang neu).
      if (mesh_client::ready()) mesh_client::applyRadioParams();
    }
    Serial.printf("[CON] Mesh RX-Sparmodus (Duty-Cycle): %s\n",
                  settings::meshEco() ? "an" : "aus");
    return;
  }
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
  if (strcmp(line, "gauge") == 0)           { battery::dumpGauge(); return; }
  if (strcmp(line, "i2cscan") == 0)         { cmdI2cScan(); return; }
  if (strcmp(line, "ls") == 0)              { cmdLs("/"); return; }
  if (strncmp(line, "ls ", 3) == 0)         { cmdLs(line + 3); return; }
  if (strncmp(line, "cat ", 4) == 0)        { cmdCat(line + 4); return; }
  if (strncmp(line, "rm ", 3) == 0)         { cmdRm(line + 3); return; }
  if (strcmp(line, "books") == 0)           { reader_app::debugScan(); return; }
  if (strcmp(line, "notes") == 0)           { notes_app::debugSmoke(); return; }
  if (strcmp(line, "wifi list") == 0) {
    int n = settings::wifiCount();
    if (n == 0) { Serial.println("[CON] Keine WLAN-Profile"); return; }
    for (int i = 0; i < n; i++) {
      char ssid[33], pass[65];
      settings::wifiSsidAt(i, ssid, sizeof(ssid));
      settings::wifiPassAt(i, pass, sizeof(pass));
      Serial.printf("[CON]   %d: '%s' (Passwort %s)\n", i, ssid,
                    pass[0] ? "gesetzt" : "-");
    }
    return;
  }
  if (strncmp(line, "wifi ssid ", 10) == 0) {
    settings::setWifiSsid(line + 10);
    Serial.printf("[CON] WLAN-SSID (Profil 0) gesetzt: '%s'\n", line + 10);
    return;
  }
  if (strncmp(line, "wifi pass ", 10) == 0) {
    settings::setWifiPass(line + 10);
    Serial.println("[CON] WLAN-Passwort (Profil 0) gesetzt");
    return;
  }
  if (strncmp(line, "wifi add ", 9) == 0) {
    // "wifi add <name> [pw]" — erstes Leerzeichen trennt SSID vom optionalen Passwort.
    char arg[130];
    strncpy(arg, line + 9, sizeof(arg) - 1); arg[sizeof(arg) - 1] = '\0';
    char* sp = strchr(arg, ' ');
    const char* pw = "";
    if (sp) { *sp = '\0'; pw = sp + 1; }
    if (!arg[0]) { Serial.println("[CON] Nutzung: wifi add <name> [pw]"); return; }
    if (settings::wifiCount() >= settings::kMaxWifiProfiles) {
      Serial.println("[CON] Maximale Profilzahl erreicht"); return;
    }
    if (settings::wifiSet(settings::wifiCount(), arg, pw))
      Serial.printf("[CON] WLAN-Profil angelegt: '%s'\n", arg);
    return;
  }
  if (strncmp(line, "wifi del ", 9) == 0) {
    int idx = atoi(line + 9);
    if (idx < 0 || idx >= settings::wifiCount()) { Serial.println("[CON] Ungueltiger Index"); return; }
    settings::wifiRemove(idx);
    Serial.printf("[CON] WLAN-Profil %d geloescht\n", idx);
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
  if (strncmp(line, "ollama url ", 11) == 0) {
    settings::setAiUrl(line + 11);
    Serial.printf("[CON] Ollama-URL gesetzt: '%s'\n", line + 11);
    return;
  }
  if (strncmp(line, "ollama model ", 13) == 0) {
    settings::setAiModel(line + 13);
    Serial.printf("[CON] Ollama-Modell gesetzt: '%s'\n", line + 13);
    return;
  }
  if (strcmp(line, "ollama on") == 0)  { settings::setAiEnabled(true);  Serial.println("[CON] Notiz-KI EIN"); return; }
  if (strcmp(line, "ollama off") == 0) { settings::setAiEnabled(false); Serial.println("[CON] Notiz-KI AUS"); return; }
  if (strcmp(line, "ollama test") == 0) {
    char msg[64];
    bool ok = notes_ai::ping(msg, sizeof(msg));
    Serial.printf("[CON] Ollama-Test: %s (%s)\n", ok ? "OK" : "Fehler", msg);
    return;
  }
  if (strcmp(line, "ollama") == 0) {
    char url[128]; settings::aiUrl(url, sizeof(url));
    char mod[48];  settings::aiModel(mod, sizeof(mod));
    Serial.printf("[CON] Notiz-KI=%s URL='%s' Modell='%s'; %d Notiz(en) offen\n",
                  settings::aiEnabled() ? "an" : "aus", url, mod, notes_ai::pendingCount());
    return;
  }
  if (strcmp(line, "ollama flush") == 0) {
    char msg[64];
    notes_ai::flushNow(msg, sizeof(msg));
    Serial.printf("[CON] Ollama-Flush: %s\n", msg);
    return;
  }
  if (strncmp(line, "podcast feed ", 13) == 0) {
    bool ok = podcast::addFeed(line + 13, nullptr);
    Serial.printf("[CON] Feed %s: %s\n", ok ? "hinzugefuegt" : "Fehler", line + 13);
    return;
  }
  if (strncmp(line, "podcast rm ", 11) == 0) {
    int idx = atoi(line + 11);
    Serial.printf("[CON] Feed %d %s\n", idx, podcast::removeFeed(idx) ? "entfernt" : "Fehler");
    return;
  }
  if (strcmp(line, "podcast on") == 0)  { settings::setPodcastAutoSync(true);  Serial.println("[CON] Podcast-Auto-Sync EIN"); return; }
  if (strcmp(line, "podcast off") == 0) { settings::setPodcastAutoSync(false); Serial.println("[CON] Podcast-Auto-Sync AUS"); return; }
  if (strcmp(line, "podcast sync") == 0) {
    char msg[64];
    podcast::syncAll(msg, sizeof(msg), nullptr);
    Serial.printf("[CON] Podcast-Sync: %s\n", msg);
    return;
  }
  if (strcmp(line, "podcast") == 0) {
    int n = podcast::feedCount();
    Serial.printf("[CON] Podcast Auto-Sync=%s, %d Feed(s):\n",
                  settings::podcastAutoSync() ? "an" : "aus", n);
    for (int i = 0; i < n; ++i) {
      podcast::Feed f;
      if (!podcast::feed(i, &f)) continue;
      podcast::Local loc;
      bool have = podcast::localEpisode(f, &loc);
      Serial.printf("[CON]   [%d] %s  (%s)\n", i, f.name,
                    have ? loc.title : "keine Folge");
    }
    return;
  }
  // --- Kalender (iCal, read-only) ---
  if (strncmp(line, "cal feed ", 9) == 0) {
    bool ok = calendar::addFeed(line + 9);
    Serial.printf("[CON] Kalender-Feed %s: %s\n", ok ? "hinzugefuegt" : "Fehler", line + 9);
    return;
  }
  if (strncmp(line, "cal rm ", 7) == 0) {
    int idx = atoi(line + 7);
    Serial.printf("[CON] Feed %d %s\n", idx, calendar::removeFeed(idx) ? "entfernt" : "Fehler");
    return;
  }
  if (strncmp(line, "cal user ", 9) == 0) { settings::setCalDavUser(line + 9); Serial.printf("[CON] CalDAV-User: '%s'\n", line + 9); return; }
  if (strncmp(line, "cal pass ", 9) == 0) { settings::setCalDavPass(line + 9); Serial.println("[CON] CalDAV-Passwort gesetzt"); return; }
  if (strcmp(line, "cal on") == 0)  { settings::setCalAutoSync(true);  Serial.println("[CON] Kalender-Auto-Sync EIN"); return; }
  if (strcmp(line, "cal off") == 0) { settings::setCalAutoSync(false); Serial.println("[CON] Kalender-Auto-Sync AUS"); return; }
  if (strcmp(line, "cal sync") == 0) {
    char msg[64]; calendar::sync(msg, sizeof(msg));
    Serial.printf("[CON] Kalender-Sync: %s\n", msg);
    return;
  }
  if (strcmp(line, "cal") == 0) {
    int n = calendar::feedCount();
    Serial.printf("[CON] Kalender Auto-Sync=%s, %d Feed(s), %d Termine:\n",
                  settings::calAutoSync() ? "an" : "aus", n, calendar::count());
    for (int i = 0; i < n; ++i) { char u[160]; if (calendar::feedUrl(i, u, sizeof(u))) Serial.printf("[CON]   [%d] %s\n", i, u); }
    return;
  }
  // --- Todo (Reinschrift via Nextcloud/WebDAV) ---
  if (strncmp(line, "todo url ", 9) == 0)  { settings::setTodoUrl(line + 9);  Serial.printf("[CON] Todo-URL: '%s'\n", line + 9); return; }
  if (strncmp(line, "todo user ", 10) == 0){ settings::setTodoUser(line + 10);Serial.printf("[CON] Todo-User: '%s'\n", line + 10); return; }
  if (strncmp(line, "todo pass ", 10) == 0){ settings::setTodoPass(line + 10);Serial.println("[CON] Todo-Passwort gesetzt"); return; }
  if (strncmp(line, "todo path ", 10) == 0){ settings::setTodoPath(line + 10);Serial.printf("[CON] Todo-Pfad: '%s'\n", line + 10); return; }
  if (strcmp(line, "todo on") == 0)  { settings::setTodoEnabled(true);  Serial.println("[CON] Todo-Sync EIN"); return; }
  if (strcmp(line, "todo off") == 0) { settings::setTodoEnabled(false); Serial.println("[CON] Todo-Sync AUS"); return; }
  if (strcmp(line, "todo sync") == 0) {
    char msg[64]; reinschrift_svc::sync(msg, sizeof(msg));
    Serial.printf("[CON] Todo-Sync: %s\n", msg);
    return;
  }
  if (strcmp(line, "todo") == 0) {
    char url[160], path[160]; settings::todoUrl(url, sizeof(url)); settings::todoPath(path, sizeof(path));
    Serial.printf("[CON] Todo=%s URL='%s' Pfad='%s'; %d Aufgaben, %d offen\n",
                  settings::todoEnabled() ? "an" : "aus", url, path,
                  reinschrift_svc::count(), reinschrift_svc::pendingCount());
    return;
  }
  // --- Calibre (E-Book-Pull-Sync vom Content Server) ---
  if (strncmp(line, "calibre url ", 12) == 0)   { settings::setCalibreUrl(line + 12);   Serial.printf("[CON] Calibre-URL: '%s'\n", line + 12); return; }
  if (strncmp(line, "calibre user ", 13) == 0)  { settings::setCalibreUser(line + 13);  Serial.printf("[CON] Calibre-User: '%s'\n", line + 13); return; }
  if (strncmp(line, "calibre pass ", 13) == 0)  { settings::setCalibrePass(line + 13);  Serial.println("[CON] Calibre-Passwort gesetzt"); return; }
  if (strncmp(line, "calibre shelf ", 14) == 0) { settings::setCalibreShelf(line + 14); Serial.printf("[CON] Calibre-Regal: '%s'\n", line + 14); return; }
  if (strcmp(line, "calibre on") == 0)  { settings::setCalibreAutoSync(true);  Serial.println("[CON] Calibre-Auto-Sync EIN"); return; }
  if (strcmp(line, "calibre off") == 0) { settings::setCalibreAutoSync(false); Serial.println("[CON] Calibre-Auto-Sync AUS"); return; }
  if (strcmp(line, "calibre sync") == 0) {
    char msg[64]; calibre_books::sync(msg, sizeof(msg));
    Serial.printf("[CON] Calibre-Sync: %s\n", msg);
    return;
  }
  if (strcmp(line, "calibre") == 0) {
    char url[160], shelf[64];
    settings::calibreUrl(url, sizeof(url));
    settings::calibreShelf(shelf, sizeof(shelf));
    Serial.printf("[CON] Calibre Auto-Sync=%s URL='%s' Regal='%s'; %d Buch/Buecher gesynct\n",
                  settings::calibreAutoSync() ? "an" : "aus", url, shelf,
                  calibre_books::syncedCount());
    return;
  }
  if (strncmp(line, "ota url ", 8) == 0) {
    settings::setOtaUrl(line + 8);
    Serial.printf("[CON] OTA-Update-URL gesetzt: '%s'\n", line + 8);
    return;
  }
  if (strcmp(line, "ota check") == 0)  { ota::consoleCheck();      return; }
  if (strcmp(line, "ota update") == 0) { ota::consoleUpdate(false); return; }
  if (strcmp(line, "ota force") == 0)  { ota::consoleUpdate(true);  return; }
  if (strcmp(line, "ota") == 0) {
    char url[160]; settings::otaUrl(url, sizeof(url));
    Serial.printf("[CON] OTA: aktuell=%s  URL='%s'\n", FENNEK_VERSION, url);
    Serial.println("[CON]   'ota check' prueft, 'ota update' flasht (WLAN noetig)");
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
  if (strcmp(line, "advert flood") == 0) {
    if (ensureMesh()) { mesh_client::sendAdvertFlood(); Serial.println("[CON] Advert gesendet (flood)"); }
    return;
  }
  if (strcmp(line, "pos") == 0)             { cmdPos(nullptr); return; }
  if (strncmp(line, "pos ", 4) == 0)        { cmdPos(line + 4); return; }
  if (strcmp(line, "gps") == 0)             { cmdGps(nullptr); return; }
  if (strncmp(line, "gps ", 4) == 0)        { cmdGps(line + 4); return; }
  if (strcmp(line, "gyro") == 0)            { cmdGyro(); return; }
  if (strcmp(line, "rec") == 0)             { cmdRec(nullptr); return; }
  if (strncmp(line, "rec ", 4) == 0)        { cmdRec(line + 4); return; }
  if (strcmp(line, "recplay") == 0) {       // letzte Aufnahme abspielen (Audio-Restore-Test)
    audio::queueBegin(audio::Owner::Music);
    audio::queueAdd("/rectest.wav");
    audio::queueCommit(0);
    Serial.println("[CON] Spiele /rectest.wav ...");
    return;
  }
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
        // Blockierende Befehle (OTA, WLAN-Syncs, Tests) mit vollem Takt fahren
        // — der Governor sieht sie sonst erst nach der Rückkehr (power.h).
        power::boostLock();
        handleLine(s_line);
        power::boostUnlock();
      }
    } else if (s_len < (int)sizeof(s_line) - 1) {
      s_line[s_len++] = c;
    }
  }
}

}  // namespace console
