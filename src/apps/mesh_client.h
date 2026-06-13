// =============================================================================
// mesh_client.h — schlanker MeshCore-Client (Channels, DMs, Kontakte).
//
// Nutzt den vendored MeshCore-Stack (lib/meshcore) über BaseChatMesh — nach
// dem Vorbild von archive_legacy/examples/simple_secure_chat. Kein BLE-
// Companion. Persistenz auf SPIFFS (interner Flash, keine SPI-Bus-Konkurrenz):
// /identity, /contacts, /node_prefs.
//
// Threading: ALLE Aufrufe laufen im UI-Thread (Core 1); jeder Radio-Zugriff
// (SX1262 am geteilten HSPI) wird intern mit spiLock()/spiUnlock() gekapselt.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace mesh_client {

// Eine Nachricht im Verlaufs-Ringpuffer.
struct MsgView {
  uint8_t  kind;          // 0=Channel, 1=DM eingehend, 2=DM ausgehend
  uint8_t  ackState;      // nur kind=2: 0=-, 1=ausstehend, 2=zugestellt, 3=Timeout
  char     from[32];      // Absendername ("" bei Channel — steckt im Text)
  char     text[160];     // UTF-8
  uint32_t timestamp;
  uint8_t  contactIdx;    // nur DM: Index in der Kontaktliste (0xFF = unbekannt)
  uint8_t  channelIdx;    // nur kind=0: Kanal-Index (0=Public). Trennt die Chats.
};

// Radio + Identity + Kontakte initialisieren. false = Radio nicht gefunden.
bool begin();
bool ready();

// Mesh-Pumpe: jeden Loop-Durchlauf aufrufen (nimmt spiLock selbst).
void loop();

// Pumpe pausieren (z. B. während WiFi-Dateitransfers den SPI-Bus brauchen).
// Wirkt sofort; das Radio bleibt initialisiert. No-op solange nicht ready.
void setSuspended(bool sus);

// --- Nachrichten ---------------------------------------------------------------
int  msgCount();
bool msg(int i, MsgView& out);          // i=0 = älteste im Puffer
uint32_t changeCounter();               // erhöht sich bei jeder Änderung

// --- Senden ----------------------------------------------------------------------
bool sendChannelMsg(const char* text);  // an den Public-Channel
bool sendChannelIdxMsg(int channelIdx, const char* text);  // an Kanal nach Index
bool sendDirectMsg(int contactIdx, const char* text);
void sendAdvert();                      // Zero-Hop-Advert ("ich bin hier")

// --- Hashtag-Channels (Mesh-Rheinland-Konvention) ---------------------------------
// PSK = sha256("#<name>")[:16] — öffentlich bekannter Schlüssel aus dem Namen.
// join: dem Kanal beitreten (idempotent); liefert Kanal-Index oder -1.
int  joinHashChannel(const char* name);            // name mit oder ohne '#'
bool sendHashChannelMsg(const char* name, const char* text);   // joint bei Bedarf
int  channelCount();
bool channelName(int i, char* out, size_t n);

// --- Kontakte ---------------------------------------------------------------------
int  contactCount();
bool contactName(int i, char* out, size_t n);

const char* nodeName();
void setNodeName(const char* name);     // persistiert + wirkt ab sofort

// Funkparameter aus settings::meshParams() live aufs Radio anwenden
// (Frequenz/BW/SF/CR/Präambel/Sendeleistung). No-op solange nicht ready.
void applyRadioParams();

// Radio-Statistiken (Noise-Floor dBm, empfangene/gesendete Pakete).
bool radioStats(int* noiseFloor, uint32_t* rxPkts, uint32_t* txPkts);

// Aktuelle Mesh-Uhrzeit (Unix-Epoche; aus Adverts synchronisiert).
uint32_t rtcTime();

}  // namespace mesh_client
