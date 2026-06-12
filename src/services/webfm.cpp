// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "webfm.h"
#include "webfm_html.h"
#include "core/board.h"
#include "core/settings.h"
#include "services/audio.h"
#include "apps/mesh_client.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <string.h>

namespace {

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr size_t   kMaxPath = 192;      // FAT-Pfadkonvention des Projekts

webfm::State s_state = webfm::State::OFF;
WebServer*   s_server = nullptr;        // lazy — kostet erst bei Nutzung RAM
uint32_t     s_connectT0 = 0;
uint32_t     s_requests = 0;
char         s_ssid[33] = "";
char         s_ip[16] = "";

// Upload-Zustand (ein Upload zur Zeit — der Server ist synchron).
File s_upFile;
char s_upPath[kMaxPath] = "";
bool s_upOk = false;
int         s_upErrCode = 507;                     // HTTP-Code bei Fehlschlag
const char* s_upErr = "Upload fehlgeschlagen";     // Grund für die Antwort

void sendJsonErr(int code, const char* err) {
  char b[112];
  snprintf(b, sizeof(b), "{\"ok\":false,\"err\":\"%s\"}", err);
  s_server->send(code, "application/json", b);
}

void sendJsonOk() { s_server->send(200, "application/json", "{\"ok\":true}"); }

// "path"-Query-Parameter validieren. forWrite schützt die internen Caches
// (/.fennek) vor Lösch-/Schreibzugriffen — /meshcore ist bewusst NICHT
// geschützt (identity.hex-Upload ist ein vorgesehener Anwendungsfall).
// sendError=false für den Upload-Callback (dort darf nicht geantwortet werden).
bool argPath(char* out, size_t n, bool forWrite, bool sendError = true) {
  String p = s_server->arg("path");
  if (p.length() == 0 || p.length() >= n) {
    if (sendError) sendJsonErr(400, "Pfad fehlt oder zu lang");
    return false;
  }
  if (p[0] != '/' || p.indexOf("..") >= 0) {
    if (sendError) sendJsonErr(400, "ungueltiger Pfad");
    return false;
  }
  if (forWrite && p.startsWith("/.fennek")) {
    if (sendError) sendJsonErr(403, "geschuetzter Pfad");
    return false;
  }
  strncpy(out, p.c_str(), n - 1);
  out[n - 1] = '\0';
  return true;
}

// Minimales JSON-Escaping für Dateinamen (Quote/Backslash; Steuerzeichen raus).
void appendJsonEscaped(String& json, const char* s) {
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') { json += '\\'; json += *s; }
    else if ((uint8_t)*s >= 0x20) json += *s;
  }
}

// --- HTTP-Handler ------------------------------------------------------------
// Alle Handler laufen im UI-Thread (poll() -> handleClient()), also NIE unter
// bereits gehaltenem spiLock. Disziplin: SD nur unter dem Lock, Netz-I/O nur
// danach — bei großen Dateien gechunkt, damit E-Ink/Touch nicht verhungern.

void handleRoot() {
  s_requests++;
  s_server->send_P(200, "text/html", kWebFmHtml);
}

void handleList() {
  s_requests++;
  char p[kMaxPath];
  if (!argPath(p, sizeof(p), false)) return;
  if (!board::sdReady()) { sendJsonErr(503, "keine SD-Karte"); return; }

  String json;
  json.reserve(4096);
  json += "{\"path\":\"";
  appendJsonEscaped(json, p);
  json += "\",\"entries\":[";

  spiLock();
  File d = SD.open(p);
  if (!d || !d.isDirectory()) {
    if (d) d.close();
    spiUnlock();
    sendJsonErr(404, "kein Verzeichnis");
    return;
  }
  bool first = true;
  File f;
  while ((f = d.openNextFile())) {
    if (!first) json += ',';
    first = false;
    json += "{\"n\":\"";
    appendJsonEscaped(json, f.name());   // String-Ops = Heap, kein SPI — ok
    json += "\",\"s\":";
    json += (unsigned)f.size();
    json += ",\"d\":";
    json += f.isDirectory() ? "true" : "false";
    json += '}';
    f.close();
  }
  d.close();
  spiUnlock();

  json += "]}";
  s_server->send(200, "application/json", json);
}

void handleDownload() {
  s_requests++;
  char p[kMaxPath];
  if (!argPath(p, sizeof(p), false)) return;
  if (!board::sdReady()) { sendJsonErr(503, "keine SD-Karte"); return; }

  spiLock();
  File f = SD.open(p);
  bool ok = f && !f.isDirectory();
  uint32_t size = ok ? f.size() : 0;
  if (f && !ok) f.close();
  spiUnlock();
  if (!ok) { sendJsonErr(404, "Datei nicht gefunden"); return; }

  const char* base = strrchr(p, '/');
  base = base ? base + 1 : p;
  char cd[224];
  snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", base);
  s_server->sendHeader("Content-Disposition", cd);
  s_server->setContentLength(size);
  s_server->send(200, "application/octet-stream", "");

  // Gechunkt streamen: SD-Read unter dem Lock, TCP-Send danach. Das Datei-
  // Handle zwischen den Chunks offen zu halten kostet keinen Buszugriff
  // (gleiches Muster wie der Audio-Read-Ahead).
  WiFiClient client = s_server->client();
  static uint8_t buf[4096];
  while (client.connected()) {
    spiLock();
    int rd = f.read(buf, sizeof(buf));
    spiUnlock();
    if (rd <= 0) break;
    size_t off = 0;
    while (off < (size_t)rd) {
      size_t wr = client.write(buf + off, rd - off);
      if (wr == 0) break;          // TCP-Fehler/Abbruch
      off += wr;
    }
    if (off < (size_t)rd) break;
  }
  spiLock();
  f.close();
  spiUnlock();
}

// Upload-Daten-Callback: wird vom WebServer pro Multipart-Häppchen (~1,4 KB)
// aufgerufen. Antworten sendet erst der Abschluss-Handler (handleUploadDone).
void handleUploadData() {
  HTTPUpload& up = s_server->upload();

  if (up.status == UPLOAD_FILE_START) {
    s_upOk = false;
    s_upPath[0] = '\0';
    s_upErrCode = 507;
    s_upErr = "Upload fehlgeschlagen";
    char dir[kMaxPath];
    if (!argPath(dir, sizeof(dir), true, false)) {
      s_upErrCode = 403;
      s_upErr = "Zielpfad ungueltig oder geschuetzt (/.fennek)";
      return;
    }
    if (!board::sdReady()) {
      s_upErrCode = 503;
      s_upErr = "keine SD-Karte";
      return;
    }
    // Dateiname auf den Basename reduzieren (Browser können Pfade mitsenden).
    const char* name = up.filename.c_str();
    const char* slash = strrchr(name, '/');
    if (slash) name = slash + 1;
    if (!name[0]) { s_upErrCode = 400; s_upErr = "Dateiname fehlt"; return; }
    int n = snprintf(s_upPath, sizeof(s_upPath), "%s/%s",
                     (strcmp(dir, "/") == 0) ? "" : dir, name);
    if (n <= 0 || n >= (int)sizeof(s_upPath)) {
      s_upPath[0] = '\0';
      s_upErrCode = 400;
      s_upErr = "Pfad zu lang";
      return;
    }
    spiLock();
    s_upFile = SD.open(s_upPath, FILE_WRITE);
    spiUnlock();
    if (s_upFile) {
      s_upOk = true;
      Serial.printf("[WEBFM] Upload startet: %s\n", s_upPath);
    } else {
      s_upErr = "Datei nicht anlegbar (Zielordner vorhanden?)";
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!s_upOk || !s_upFile) return;
    spiLock();
    size_t wr = s_upFile.write(up.buf, up.currentSize);
    spiUnlock();
    if (wr != up.currentSize) {     // SD voll/Schreibfehler -> abbrechen
      spiLock();
      s_upFile.close();
      SD.remove(s_upPath);
      spiUnlock();
      s_upOk = false;
      s_upErr = "Schreibfehler (SD voll?)";
      Serial.printf("[WEBFM] Upload-Schreibfehler: %s\n", s_upPath);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_upFile) {
      spiLock();
      s_upFile.close();
      spiUnlock();
      Serial.printf("[WEBFM] Upload fertig: %s (%u Bytes)\n",
                    s_upPath, (unsigned)up.totalSize);
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (s_upFile) {
      spiLock();
      s_upFile.close();
      if (s_upPath[0]) SD.remove(s_upPath);
      spiUnlock();
    }
    s_upOk = false;
    Serial.println("[WEBFM] Upload abgebrochen");
  }
}

void handleUploadDone() {
  s_requests++;
  if (s_upOk) sendJsonOk();
  else        sendJsonErr(s_upErrCode, s_upErr);
}

void handleDelete() {
  s_requests++;
  char p[kMaxPath];
  if (!argPath(p, sizeof(p), true)) return;
  if (!board::sdReady()) { sendJsonErr(503, "keine SD-Karte"); return; }

  spiLock();
  File f = SD.open(p);
  if (!f) {
    spiUnlock();
    sendJsonErr(404, "nicht gefunden");
    return;
  }
  bool isDir = f.isDirectory();
  f.close();
  bool ok = isDir ? SD.rmdir(p) : SD.remove(p);   // rmdir nur wenn leer
  spiUnlock();

  if (ok) { Serial.printf("[WEBFM] Gelöscht: %s\n", p); sendJsonOk(); }
  else if (isDir) sendJsonErr(409, "Verzeichnis nicht leer");
  else            sendJsonErr(500, "Loeschen fehlgeschlagen");
}

void handleMkdir() {
  s_requests++;
  char p[kMaxPath];
  if (!argPath(p, sizeof(p), true)) return;
  if (!board::sdReady()) { sendJsonErr(503, "keine SD-Karte"); return; }

  spiLock();
  bool exists = SD.exists(p);
  bool ok = !exists && SD.mkdir(p);
  spiUnlock();

  if (ok) sendJsonOk();
  else if (exists) sendJsonErr(409, "existiert bereits");
  else             sendJsonErr(500, "mkdir fehlgeschlagen");
}

void ensureServer() {
  if (s_server) return;
  s_server = new WebServer(80);
  s_server->on("/",             HTTP_GET,  handleRoot);
  s_server->on("/api/list",     HTTP_GET,  handleList);
  s_server->on("/api/download", HTTP_GET,  handleDownload);
  s_server->on("/api/upload",   HTTP_POST, handleUploadDone, handleUploadData);
  s_server->on("/api/delete",   HTTP_POST, handleDelete);
  s_server->on("/api/mkdir",    HTTP_POST, handleMkdir);
  s_server->onNotFound([]() { sendJsonErr(404, "unbekannter Endpunkt"); });
}

}  // namespace

namespace webfm {

bool start() {
  if (s_state == State::CONNECTING || s_state == State::RUNNING) return true;

  char pass[65];
  settings::wifiSsid(s_ssid, sizeof(s_ssid));
  settings::wifiPass(pass, sizeof(pass));
  if (!s_ssid[0]) {
    Serial.println("[WEBFM] Keine SSID konfiguriert ('wifi ssid <name>')");
    return false;
  }

  // Invarianten: WiFi-Task (Core 0, hohe Prio) verdrängt den Audio-Task,
  // und Dateitransfers brauchen den SPI-Bus — Audio aus, Mesh-Pumpe pausieren.
  audio::stop();
  mesh_client::setSuspended(true);

  WiFi.persistent(false);   // WiFi-Lib soll nicht selbst ins NVS schreiben
  WiFi.mode(WIFI_STA);
  WiFi.begin(s_ssid, pass);
  s_connectT0 = millis();
  s_requests = 0;
  s_ip[0] = '\0';
  s_state = State::CONNECTING;
  Serial.printf("[WEBFM] Verbinde mit '%s' ... (freier Heap: %u KB)\n",
                s_ssid, (unsigned)(ESP.getFreeHeap() / 1024));
  return true;
}

void stop() {
  if (s_state == State::OFF) return;
  if (s_server) s_server->stop();
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  mesh_client::setSuspended(false);
  s_state = State::OFF;
  s_ip[0] = '\0';
  Serial.printf("[WEBFM] WiFi aus (freier Heap: %u KB)\n",
                (unsigned)(ESP.getFreeHeap() / 1024));
}

void poll() {
  if (s_state == State::CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      snprintf(s_ip, sizeof(s_ip), "%s", WiFi.localIP().toString().c_str());
      ensureServer();
      s_server->begin();
      if (MDNS.begin("fennek")) MDNS.addService("http", "tcp", 80);
      s_state = State::RUNNING;
      Serial.printf("[WEBFM] Verbunden: http://%s/ (http://fennek.local/)\n", s_ip);
    } else if (millis() - s_connectT0 > kConnectTimeoutMs) {
      Serial.println("[WEBFM] Verbindung fehlgeschlagen (Timeout) — WiFi aus");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      mesh_client::setSuspended(false);
      s_state = State::FAILED;
    }
  } else if (s_state == State::RUNNING) {
    s_server->handleClient();
  }
}

State state() { return s_state; }

void ipStr(char* out, size_t n) {
  strncpy(out, s_ip, n - 1);
  out[n - 1] = '\0';
}

void ssid(char* out, size_t n) {
  if (s_state == State::OFF) settings::wifiSsid(s_ssid, sizeof(s_ssid));
  strncpy(out, s_ssid, n - 1);
  out[n - 1] = '\0';
}

uint32_t requestCount() { return s_requests; }

}  // namespace webfm
