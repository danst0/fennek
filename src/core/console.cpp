#include "console.h"
#include "config.h"
#include "core/battery.h"
#include "core/board.h"
#include "core/settings.h"
#include "apps/mesh_client.h"

#include <Arduino.h>
#include <SD.h>
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
  Serial.println("[CON]   mesh init         - Mesh-Radio initialisieren");
  Serial.println("[CON]   advert            - Zero-Hop-Advert senden");
  Serial.println("[CON]   public <text>     - Nachricht an Public-Channel");
  Serial.println("[CON]   dm <idx> <text>   - Direktnachricht an Kontakt #idx");
  Serial.println("[CON]   contacts          - Kontaktliste");
  Serial.println("[CON]   channels          - beigetretene Kanäle");
  Serial.println("[CON]   join <name>       - Hashtag-Kanal beitreten (z.B. join test)");
  Serial.println("[CON]   chan <name> <txt> - Nachricht an Hashtag-Kanal");
  Serial.println("[CON]   msgs              - Nachrichten-Verlauf dumpen");
  Serial.println("[CON]   meshlog           - Ende des SD-Nachrichten-Logs zeigen");
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

void cmdStatus() {
  Serial.printf("[CON] Heap=%uKB PSRAM=%uKB Akku=%umV/%u%%%s SD=%s\n",
                (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(ESP.getFreePsram() / 1024),
                battery::milliVolts(), battery::percent(),
                battery::charging() ? "+" : "",
                board::sdReady() ? "ja" : "nein");
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
  Serial.printf("[CON] Kontakte=%d Nachrichten=%d RTC=%lu\n",
                mesh_client::contactCount(), mesh_client::msgCount(),
                (unsigned long)mesh_client::rtcTime());
}

void cmdContacts() {
  if (!mesh_client::ready()) { Serial.println("[CON] Mesh nicht initialisiert"); return; }
  int n = mesh_client::contactCount();
  Serial.printf("[CON] %d Kontakt(e):\n", n);
  for (int i = 0; i < n; i++) {
    char nm[40];
    mesh_client::contactName(i, nm, sizeof(nm));
    Serial.printf("[CON]   [%d] %s\n", i, nm);
  }
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

  if (strcmp(line, "help") == 0)            { cmdHelp(); return; }
  if (strcmp(line, "status") == 0)          { cmdStatus(); return; }
  if (strcmp(line, "mesh init") == 0)       { ensureMesh(); return; }
  if (strcmp(line, "contacts") == 0)        { cmdContacts(); return; }
  if (strcmp(line, "msgs") == 0)            { cmdMsgs(); return; }
  if (strcmp(line, "meshlog") == 0)         { cmdMeshLog(); return; }
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
