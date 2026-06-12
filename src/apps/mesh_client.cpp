#include "mesh_client.h"
#include "config.h"
#include "core/board.h"
#include "core/battery.h"
#include "core/settings.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>

#include <Mesh.h>
#include <SHA256.h>     // rweather/Crypto (Hashtag-Channel-PSK)
#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/radiolib/CustomSX1262.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>

namespace {

// --- Timing-Konstanten (aus simple_secure_chat) --------------------------------
constexpr uint32_t SEND_TIMEOUT_BASE_MILLIS        = 500;
constexpr float    FLOOD_SEND_TIMEOUT_FACTOR       = 16.0f;
constexpr float    DIRECT_SEND_PERHOP_FACTOR       = 6.0f;
constexpr uint32_t DIRECT_SEND_PERHOP_EXTRA_MILLIS = 250;

// Andys öffentlicher Channel (MeshCore-Default).
constexpr const char* PUBLIC_GROUP_PSK = "izOH6cXN6mrJ5e26oRXNcg==";

// --- Nachrichten-Ringpuffer (PSRAM) ----------------------------------------------
constexpr int kMaxMsgs = 128;
mesh_client::MsgView* s_msgs = nullptr;
int      s_msgHead = 0;        // nächster Schreibplatz
int      s_msgLen  = 0;
uint32_t s_changes = 0;

// --- SD-Nachrichten-Log -------------------------------------------------------------
// /meshcore/messages.log: "epoch \t kind \t from \t text" — am PC lesbar.
// Ohne SD-Karte wird stillschweigend übersprungen.
constexpr const char* kLogDir  = "/meshcore";
constexpr const char* kLogPath = "/meshcore/messages.log";
constexpr const char* kLogOld  = "/meshcore/messages.old";
constexpr uint32_t    kLogRotateBytes = 256 * 1024;

// Optionale, vom Nutzer vorgegebene MeshCore-Identität von der SD-Karte.
// Format (Textdatei): Zeile 1 = Private Key (128 Hex-Zeichen / 64 Byte),
// Zeile 2 = Public Key (64 Hex-Zeichen / 32 Byte). Ist die Datei vorhanden und
// gültig, hat sie Vorrang vor der SPIFFS-Identität und wird dorthin übernommen.
constexpr const char* kIdentityPath = "/meshcore/identity.hex";

// Hex-String -> Bytes; liefert die Anzahl geschriebener Bytes oder -1 bei
// ungültigem Zeichen. Bewusst lokal (keine Lib-Abhängigkeit, nur beim Init).
int hexToBytes(uint8_t* out, int maxLen, const char* hex) {
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  int n = 0;
  for (int i = 0; hex[i] && hex[i + 1] && n < maxLen; i += 2) {
    int hi = nib(hex[i]), lo = nib(hex[i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[n++] = (uint8_t)((hi << 4) | lo);
  }
  return n;
}

// Liest die beiden Hex-Zeilen aus /meshcore/identity.hex. Der AUFRUFER hält
// spiLock (MeshClient::begin läuft komplett unter dem Lock aus
// mesh_client::begin) — hier selbst spiLock() zu nehmen wäre ein Self-Deadlock
// auf dem nicht-rekursiven Mutex (vgl. logToSd/pushMsg-Kommentar).
// true nur, wenn beide Zeilen lang genug sind.
bool readIdentityFile(char* prvOut, size_t prvLen, char* pubOut, size_t pubLen) {
  if (!board::sdReady()) return false;
  bool ok = false;
  if (SD.exists(kIdentityPath)) {
    File f = SD.open(kIdentityPath);
    if (f) {
      String l1 = f.readStringUntil('\n');
      String l2 = f.readStringUntil('\n');
      f.close();
      l1.trim();  // entfernt auch ein evtl. \r (CRLF)
      l2.trim();
      strncpy(prvOut, l1.c_str(), prvLen - 1); prvOut[prvLen - 1] = '\0';
      strncpy(pubOut, l2.c_str(), pubLen - 1); pubOut[pubLen - 1] = '\0';
      ok = (strlen(prvOut) >= 128 && strlen(pubOut) >= 64);
    }
  }
  return ok;
}

void logToSd(const mesh_client::MsgView& m) {
  if (!board::sdReady()) return;
  spiLock();
  if (!SD.exists(kLogDir)) SD.mkdir(kLogDir);

  // Rotation: zu groß -> als .old wegschieben, frisch beginnen.
  if (SD.exists(kLogPath)) {
    File f = SD.open(kLogPath);
    if (f) {
      uint32_t sz = f.size();
      f.close();
      if (sz > kLogRotateBytes) {
        if (SD.exists(kLogOld)) SD.remove(kLogOld);
        SD.rename(kLogPath, kLogOld);
      }
    }
  }

  File f = SD.open(kLogPath, FILE_APPEND);
  if (f) {
    f.printf("%lu\t%u\t%s\t%s\n", (unsigned long)m.timestamp, m.kind,
             m.from, m.text);
    f.close();
  }
  spiUnlock();
}

// SD-Schreiben passiert NICHT in pushMsg: pushMsg wird u. a. aus dem
// Mesh-Loop heraus aufgerufen, der bereits spiLock() hält — ein direktes
// logToSd() (nimmt spiLock erneut) wäre ein Self-Deadlock auf dem
// nicht-rekursiven Mutex. Stattdessen nur markieren; flushLog() schreibt
// später außerhalb des Locks (mesh_client::loop).
bool s_needLog[kMaxMsgs] = {};

void pushMsg(const mesh_client::MsgView& m, bool log = true) {
  if (!s_msgs) return;
  s_msgs[s_msgHead] = m;
  s_needLog[s_msgHead] = log;
  s_msgHead = (s_msgHead + 1) % kMaxMsgs;
  if (s_msgLen < kMaxMsgs) s_msgLen++;
  s_changes++;
}

// Ausstehende Log-Einträge auf SD schreiben (von alt nach neu).
// Aufrufer darf spiLock NICHT halten.
void flushLog() {
  if (!s_msgs) return;
  for (int i = 0; i < s_msgLen; i++) {
    int idx = (s_msgHead - s_msgLen + i + kMaxMsgs) % kMaxMsgs;
    if (s_needLog[idx]) {
      s_needLog[idx] = false;
      logToSd(s_msgs[idx]);
    }
  }
}

// Beim Mesh-Start die letzten Nachrichten vom SD-Log in den Verlauf laden
// (Chat-Verlauf überlebt Reboots). Liest nur das Datei-Ende (<=16 KB).
void loadHistoryFromSd() {
  if (!board::sdReady()) return;
  constexpr size_t kTail = 16384;
  constexpr int    kLoad = 50;

  char* buf = (char*)heap_caps_malloc(kTail + 1, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (char*)malloc(kTail + 1);
  if (!buf) return;

  int rd = 0;
  bool truncated = false;   // Lese-Start mitten in der Datei -> 1. Zeile angeschnitten
  spiLock();
  if (!SD.exists(kLogPath)) { spiUnlock(); free(buf); return; }
  File f = SD.open(kLogPath);
  if (f) {
    uint32_t sz = f.size();
    uint32_t from = (sz > kTail) ? sz - kTail : 0;
    truncated = (from > 0);
    f.seek(from);
    rd = f.read((uint8_t*)buf, kTail);
    f.close();
  }
  spiUnlock();
  if (rd <= 0) { free(buf); return; }
  buf[rd] = '\0';

  // Zeilen einsammeln (Schiebefenster über die letzten 64).
  char* lines[64];
  int nLines = 0;
  char* p = buf;
  bool skipFirst = truncated;
  while (p && *p) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    if (skipFirst) {
      skipFirst = false;            // angeschnittene erste Zeile verwerfen
    } else if (*p) {
      if (nLines < 64) lines[nLines++] = p;
      else {
        memmove(lines, lines + 1, sizeof(lines[0]) * 63);
        lines[63] = p;
      }
    }
    p = nl ? nl + 1 : nullptr;
  }

  int start = (nLines > kLoad) ? nLines - kLoad : 0;
  int loaded = 0;
  for (int i = start; i < nLines; i++) {
    // Format: epoch \t kind \t from \t text
    char* l = lines[i];
    char* t1 = strchr(l, '\t');  if (!t1) continue;
    char* t2 = strchr(t1 + 1, '\t'); if (!t2) continue;
    char* t3 = strchr(t2 + 1, '\t'); if (!t3) continue;
    *t1 = *t2 = *t3 = '\0';

    mesh_client::MsgView m{};
    m.timestamp  = (uint32_t)strtoul(l, nullptr, 10);
    m.kind       = (uint8_t)atoi(t1 + 1);
    m.ackState   = 0;       // nach Reboot unbekannt
    m.contactIdx = 0xFF;    // Kontakt-Zuordnung nach Reboot nicht garantiert
    strncpy(m.from, t2 + 1, sizeof(m.from) - 1);
    strncpy(m.text, t3 + 1, sizeof(m.text) - 1);
    if (m.kind > 2) continue;
    pushMsg(m, /*log=*/false);
    loaded++;
  }
  free(buf);
  if (loaded) Serial.printf("[MESH] %d Nachricht(en) aus SD-Log geladen\n", loaded);
}

// Base64-Encoder (nur fürs Hashtag-Channel-PSK; base64.hpp ist nicht
// include-safe — BaseChatMesh.cpp definiert dessen Funktionen bereits).
void b64encode(const uint8_t* in, int len, char* out) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int o = 0;
  for (int i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < len) v |= in[i + 2];
    out[o++] = k[(v >> 18) & 63];
    out[o++] = k[(v >> 12) & 63];
    out[o++] = (i + 1 < len) ? k[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < len) ? k[v & 63] : '=';
  }
  out[o] = '\0';
}

// --- MainBoard-Adapter (statt schwergewichtigem ESP32Board) ------------------------
class TDeckMeshBoard : public mesh::MainBoard {
public:
  uint16_t getBattMilliVolts() override { return battery::milliVolts(); }
  uint8_t  getBatteryPercent() override { return battery::percent(); }
  const char* getManufacturerName() const override { return "LilyGo T-Deck Pro"; }
  void reboot() override { ESP.restart(); }
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

struct NodePrefs {
  float  airtime_factor;
  char   node_name[32];
  double node_lat, node_lon;
  float  freq;
  uint8_t tx_power_dbm;
  uint8_t unused[3];
};

// --- Statische Mesh-Objekte ----------------------------------------------------------
TDeckMeshBoard       s_board;
CustomSX1262*        s_radio = nullptr;
CustomSX1262Wrapper* s_radioDrv = nullptr;
StdRNG               s_rng;
VolatileRTCClock     s_rtc;
SimpleMeshTables*    s_tables = nullptr;

class MeshClient;
MeshClient* s_mesh = nullptr;
bool s_ready = false;

class MeshClient : public BaseChatMesh {
  fs::FS* _fs;
  NodePrefs _prefs;
  ChannelDetails* _public;
  uint32_t _expectedAck;
  int _expectedAckMsg;     // Ringpuffer-Slot der Nachricht, auf die der ACK gehört

  void loadContacts() {
    if (!_fs->exists("/contacts")) return;
    File file = _fs->open("/contacts");
    if (!file) return;
    bool full = false;
    while (!full) {
      ContactInfo c;
      uint8_t pub_key[32];
      uint8_t unused;
      uint32_t reserved;
      bool ok = (file.read(pub_key, 32) == 32);
      ok = ok && (file.read((uint8_t*)&c.name, 32) == 32);
      ok = ok && (file.read(&c.type, 1) == 1);
      ok = ok && (file.read(&c.flags, 1) == 1);
      ok = ok && (file.read(&unused, 1) == 1);
      ok = ok && (file.read((uint8_t*)&reserved, 4) == 4);
      ok = ok && (file.read((uint8_t*)&c.out_path_len, 1) == 1);
      ok = ok && (file.read((uint8_t*)&c.last_advert_timestamp, 4) == 4);
      ok = ok && (file.read(c.out_path, 64) == 64);
      c.gps_lat = c.gps_lon = 0;
      if (!ok) break;
      c.id = mesh::Identity(pub_key);
      // lastmod = Advert-Zeit, damit bootstrapRTCfromContacts() die Uhr
      // setzen kann (lastmod=0 ließe die RTC auf Mai 2024 stehen — Bots
      // verwerfen dann unsere Nachrichten als veraltet).
      c.lastmod = c.last_advert_timestamp;
      if (!addContact(c)) full = true;
    }
    file.close();
  }

  void saveContacts() {
    File file = _fs->open("/contacts", "w", true);
    if (!file) return;
    ContactsIterator iter;
    ContactInfo c;
    uint8_t unused = 0;
    uint32_t reserved = 0;
    while (iter.hasNext(this, c)) {
      bool ok = (file.write(c.id.pub_key, 32) == 32);
      ok = ok && (file.write((uint8_t*)&c.name, 32) == 32);
      ok = ok && (file.write(&c.type, 1) == 1);
      ok = ok && (file.write(&c.flags, 1) == 1);
      ok = ok && (file.write(&unused, 1) == 1);
      ok = ok && (file.write((uint8_t*)&reserved, 4) == 4);
      ok = ok && (file.write((uint8_t*)&c.out_path_len, 1) == 1);
      ok = ok && (file.write((uint8_t*)&c.last_advert_timestamp, 4) == 4);
      ok = ok && (file.write(c.out_path, 64) == 64);
      if (!ok) break;
    }
    file.close();
  }

  int indexOfContact(const ContactInfo& c) {
    ContactsIterator iter;
    ContactInfo tmp;
    int i = 0;
    while (iter.hasNext(this, tmp)) {
      if (memcmp(tmp.id.pub_key, c.id.pub_key, PUB_KEY_SIZE) == 0) return i;
      i++;
    }
    return -1;
  }

protected:
  float getAirtimeBudgetFactor() const override { return _prefs.airtime_factor; }
  int calcRxDelay(float score, uint32_t air_time) const override { return 0; }
  bool allowPacketForward(const mesh::Packet* packet) override { return true; }
  uint8_t getPathHashSize() const override { return 1; }

  void onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len,
                           const uint8_t* path) override {
    Serial.printf("[MESH] Advert von '%s'%s\n", contact.name, is_new ? " (neu)" : "");
    // Uhr opportunistisch aus (signierten) Adverts synchronisieren — das
    // Gerät hat keine gepufferte RTC; ohne echte Zeit tragen unsere
    // Nachrichten veraltete Timestamps und werden von Bots ignoriert.
    uint32_t t = contact.last_advert_timestamp;
    if (t > getRTCClock()->getCurrentTime()) {
      getRTCClock()->setCurrentTime(t + 1);
      Serial.printf("[MESH] Uhr aus Advert gesetzt: %lu\n", (unsigned long)t);
    }
    saveContacts();
    s_changes++;
  }

  void onContactPathUpdated(const ContactInfo& contact) override {
    saveContacts();
    s_changes++;
  }

  ContactInfo* processAck(const uint8_t* data) override {
    if (_expectedAck && memcmp(data, &_expectedAck, 4) == 0) {
      _expectedAck = 0;
      if (s_msgs && _expectedAckMsg >= 0) {
        s_msgs[_expectedAckMsg].ackState = 2;   // zugestellt
        s_changes++;
      }
      _expectedAckMsg = -1;
    }
    return NULL;
  }

  void onSendTimeout() override {
    if (s_msgs && _expectedAckMsg >= 0 && s_msgs[_expectedAckMsg].ackState == 1) {
      s_msgs[_expectedAckMsg].ackState = 3;     // Timeout
      s_changes++;
    }
    _expectedAck = 0;
    _expectedAckMsg = -1;
  }

  void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt,
                     uint32_t sender_timestamp, const char* text) override {
    mesh_client::MsgView m{};
    m.kind = 1;
    strncpy(m.from, from.name, sizeof(m.from) - 1);
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.timestamp = sender_timestamp;
    int idx = indexOfContact(from);
    m.contactIdx = (idx >= 0) ? (uint8_t)idx : 0xFF;
    pushMsg(m);
    Serial.printf("[MESH] DM von '%s': %s\n", from.name, text);
  }

  void onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                            uint32_t timestamp, const char* text) override {
    mesh_client::MsgView m{};
    m.kind = 0;
    // Nicht-Public-Kanäle mit Kanalnamen prefixen ("[#test] Sender: Text").
    int idx = findChannelIdx(channel);
    ChannelDetails cd;
    if (idx > 0 && getChannel(idx, cd)) {
      snprintf(m.text, sizeof(m.text), "[%s] %s", cd.name, text);
    } else {
      strncpy(m.text, text, sizeof(m.text) - 1);   // Format: "<Sender>: <Text>"
    }
    m.timestamp = timestamp;
    m.contactIdx = 0xFF;
    m.channelIdx = (uint8_t)(idx < 0 ? 0 : idx);   // Kanal-Zuordnung für die Chat-Trennung
    pushMsg(m);
    Serial.printf("[MESH] Channel: %s\n", m.text);
  }

  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*,
                           const char*) override {}
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t,
                           uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}

  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override {
    return SEND_TIMEOUT_BASE_MILLIS + (uint32_t)(FLOOD_SEND_TIMEOUT_FACTOR * pkt_airtime_millis);
  }
  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override {
    return SEND_TIMEOUT_BASE_MILLIS +
           (uint32_t)((pkt_airtime_millis * DIRECT_SEND_PERHOP_FACTOR +
                       DIRECT_SEND_PERHOP_EXTRA_MILLIS) * (path_len + 1));
  }

public:
  MeshClient(mesh::Radio& radio, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
      : BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc, *new StaticPoolPacketManager(16), tables) {
    memset(&_prefs, 0, sizeof(_prefs));
    _prefs.airtime_factor = 2.0f;
    strcpy(_prefs.node_name, "T-Deck");
    _prefs.freq = LORA_FREQ;
    _prefs.tx_power_dbm = LORA_TX_POWER;
    _public = NULL;
    _expectedAck = 0;
    _expectedAckMsg = -1;
  }

  const char* name() const { return _prefs.node_name; }
  float freqPref() const { return _prefs.freq; }
  uint8_t txPowerPref() const { return _prefs.tx_power_dbm; }

  void begin(fs::FS& fs) {
    _fs = &fs;
    BaseChatMesh::begin();
    // PFLICHT vor loadContacts/Advert-Empfang: Kontakt-Array im PSRAM anlegen
    // (im Konstruktor nicht möglich — PSRAM ist dort noch nicht verfügbar).
    initContacts();

    IdentityStore store(fs, "/identity");

    // 1) Vorrang: vom Nutzer auf der SD vorgegebene Identität (kIdentityPath).
    //    Ist sie gültig, wird sie in den SPIFFS-Store übernommen (überlebt damit
    //    auch ohne SD) und überschreibt eine evtl. vorhandene Zufalls-Identität.
    bool fromSd = false;
    {
      char prvHex[160] = {0}, pubHex[96] = {0};
      if (readIdentityFile(prvHex, sizeof(prvHex), pubHex, sizeof(pubHex))) {
        uint8_t prv[64];
        if (hexToBytes(prv, 64, prvHex) == 64 &&
            mesh::LocalIdentity::validatePrivateKey(prv)) {
          self_id = mesh::LocalIdentity(prvHex, pubHex);
          store.save("_main", self_id);
          fromSd = true;
          Serial.println("[MESH] Identität von SD geladen (/meshcore/identity.hex)");
        } else {
          Serial.println("[MESH] identity.hex ungültig (Key-Prüfung fehlgeschlagen) — ignoriert");
        }
      }
    }

    // 2) Sonst aus SPIFFS laden; 3) sonst beim Erstboot neu erzeugen.
    if (!fromSd && !store.load("_main", self_id, _prefs.node_name, sizeof(_prefs.node_name))) {
      // Erstboot: Identität aus Hardware-Entropie erzeugen (kein Warten auf Serial).
      s_rng.begin((long)esp_random());
      self_id = mesh::LocalIdentity(getRNG());
      int count = 0;
      while (count < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF)) {
        self_id = mesh::LocalIdentity(getRNG());
        count++;
      }
      store.save("_main", self_id);
      Serial.println("[MESH] Neue Identität erzeugt");
    }

    // Node-Name kommt aus den NVS-Settings (Settings-App), nicht aus SPIFFS.
    settings::meshName(_prefs.node_name, sizeof(_prefs.node_name));

    loadContacts();
    bootstrapRTCfromContacts();
    _public = addChannel("Public", PUBLIC_GROUP_PSK);
  }

  void setName(const char* name) {
    strncpy(_prefs.node_name, name, sizeof(_prefs.node_name) - 1);
    _prefs.node_name[sizeof(_prefs.node_name) - 1] = '\0';
  }

  // Hashtag-Channel beitreten (Mesh-Rheinland-Konvention):
  // PSK = sha256("#<name>")[:16]. Idempotent; liefert Kanal-Index oder -1.
  int joinHash(const char* nameIn) {
    char full[40];
    snprintf(full, sizeof(full), "%s%s", (nameIn[0] == '#') ? "" : "#", nameIn);

    // Schon beigetreten?
    ChannelDetails cd;
    for (int i = 0; getChannel(i, cd) && cd.name[0]; i++) {
      if (strcasecmp(cd.name, full) == 0) return i;
    }

    uint8_t digest[32];
    SHA256 sha;
    sha.update(full, strlen(full));
    sha.finalize(digest, sizeof(digest));

    char b64[32];
    b64encode(digest, 16, b64);   // PSK = erste 16 Byte von sha256("#name")

    ChannelDetails* added = addChannel(full, b64);
    if (!added) return -1;
    int idx = findChannelIdx(added->channel);
    Serial.printf("[MESH] Kanal '%s' beigetreten (Index %d)\n", full, idx);
    return idx;
  }

  // Nachricht an Kanal-Index senden (0 = Public, sonst Hashtag-Kanäle).
  bool sendToChannel(int idx, const char* text) {
    ChannelDetails cd;
    if (!getChannel(idx, cd)) return false;
    return sendGroupMessage(getRTCClock()->getCurrentTime(), cd.channel,
                            _prefs.node_name, text, strlen(text));
  }

  bool getChannelInfo(int i, ChannelDetails& cd) { return getChannel(i, cd); }

  void sendSelfAdvertNow() {
    auto pkt = createSelfAdvert(_prefs.node_name, _prefs.node_lat, _prefs.node_lon);
    if (pkt) sendZeroHop(pkt);
  }

  bool sendPublic(const char* text) {
    if (!_public) return false;
    mesh::GroupChannel ch = _public->channel;
    return sendGroupMessage(getRTCClock()->getCurrentTime(), ch,
                            _prefs.node_name, text, strlen(text));
  }

  // DM senden; liefert Ringpuffer-Index der Sende-Nachricht (-1 bei Fehler).
  int sendDirect(int contactIdx, const char* text) {
    ContactInfo c;
    if (!getContactByIdx(contactIdx, c)) return -1;
    uint32_t est_timeout;
    uint32_t ack = 0;
    int result = sendMessage(c, getRTCClock()->getCurrentTime(), 0, text, ack, est_timeout);
    if (result == MSG_SEND_FAILED) return -1;

    mesh_client::MsgView m{};
    m.kind = 2;
    m.ackState = 1;   // ausstehend
    strncpy(m.from, c.name, sizeof(m.from) - 1);
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.timestamp = getRTCClock()->getCurrentTime();
    m.contactIdx = (uint8_t)contactIdx;
    int slot = s_msgHead;
    pushMsg(m);
    _expectedAck = ack;
    _expectedAckMsg = slot;
    return slot;
  }
};

}  // namespace

namespace mesh_client {

bool begin() {
  if (s_ready) return true;

  s_msgs = (MsgView*)heap_caps_calloc(kMaxMsgs, sizeof(MsgView), MALLOC_CAP_SPIRAM);
  if (!s_msgs) s_msgs = (MsgView*)calloc(kMaxMsgs, sizeof(MsgView));

  if (!SPIFFS.begin(true)) {
    Serial.println("[MESH] SPIFFS-Mount fehlgeschlagen");
    return false;
  }

  board::loraPower(true);

  // Radio am geteilten Bus: g_spi wird MIT übergeben (kein eigenes spi.begin).
  spiLock();
  s_radio = new CustomSX1262(new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST,
                                        PIN_LORA_BUSY, g_spi,
                                        SPISettings(SPI_BUS_HZ, MSBFIRST, SPI_MODE0)));
  bool radioOk = s_radio->std_init(NULL);
  spiUnlock();
  if (!radioOk) {
    Serial.println("[MESH] Radio-Init FEHLGESCHLAGEN");
    board::loraPower(false);
    return false;
  }

  s_radioDrv = new CustomSX1262Wrapper(*s_radio, s_board);
  s_tables   = new SimpleMeshTables();
  s_rng.begin((long)esp_random());
  s_mesh = new MeshClient(*s_radioDrv, s_rng, s_rtc, *s_tables);

  spiLock();
  s_mesh->begin(SPIFFS);
  s_radioDrv->begin();
  spiUnlock();

  s_ready = true;

  // Funkparameter aus den Settings anwenden (std_init nutzte die Compile-Defaults).
  applyRadioParams();

  settings::MeshParams p = settings::meshParams();
  Serial.printf("[MESH] bereit: '%s' @ %.3f MHz SF%u BW%.1f CR4/%u %udBm (Noise-Floor-Kalibrierung läuft)\n",
                s_mesh->name(), (double)p.freqMhz, p.sf, (double)p.bwKhz, p.cr, p.txDbm);

  // Chat-Verlauf vom SD-Log wiederherstellen (falls Karte steckt).
  loadHistoryFromSd();

  // Boot-Advert mit etwas Verzögerung (Zero-Hop folgt auf Nutzeraktion).
  spiLock();
  auto pkt = s_mesh->createSelfAdvert(s_mesh->name());
  if (pkt) s_mesh->sendFlood(pkt, 1500);
  spiUnlock();
  return true;
}

bool ready() { return s_ready; }

namespace { bool s_suspended = false; }

void setSuspended(bool sus) {
  s_suspended = sus;
  if (s_ready) Serial.printf("[MESH] Pumpe %s\n", sus ? "pausiert (WiFi aktiv)" : "läuft wieder");
}

void loop() {
  if (!s_ready || s_suspended) return;
  spiLock();
  s_mesh->loop();
  spiUnlock();
  s_rtc.tick();
  // Im Mesh-Loop angefallene Nachrichten jetzt (lock-frei) auf SD loggen.
  flushLog();
}

int msgCount() { return s_msgLen; }

bool msg(int i, MsgView& out) {
  if (!s_msgs || i < 0 || i >= s_msgLen) return false;
  int idx = (s_msgHead - s_msgLen + i + kMaxMsgs) % kMaxMsgs;
  out = s_msgs[idx];
  return true;
}

uint32_t changeCounter() { return s_changes; }

bool sendChannelMsg(const char* text) {
  if (!s_ready || !text || !text[0]) return false;
  spiLock();
  bool ok = s_mesh->sendPublic(text);
  spiUnlock();
  if (ok) {
    // Eigene Nachricht in den Verlauf (Channel-Konvention "<Name>: <Text>").
    MsgView m{};
    m.kind = 0;
    snprintf(m.text, sizeof(m.text), "%s: %s", s_mesh->name(), text);
    m.timestamp = 0;
    m.contactIdx = 0xFF;
    m.channelIdx = 0;   // Public
    pushMsg(m);
  }
  return ok;
}

// An einen Kanal nach Index senden (0=Public). Nutzt die UI für die
// vereinheitlichte Chat-Ansicht: jeder Kanal ist ein eigener Chat.
bool sendChannelIdxMsg(int channelIdx, const char* text) {
  if (channelIdx <= 0) return sendChannelMsg(text);
  if (!s_ready || !text || !text[0]) return false;
  spiLock();
  bool ok = s_mesh->sendToChannel(channelIdx, text);
  spiUnlock();
  if (ok) {
    char nm[32] = "";
    channelName(channelIdx, nm, sizeof(nm));
    MsgView m{};
    m.kind = 0;
    snprintf(m.text, sizeof(m.text), "[%s] %s: %s", nm, s_mesh->name(), text);
    m.timestamp = 0;
    m.contactIdx = 0xFF;
    m.channelIdx = (uint8_t)channelIdx;
    pushMsg(m);
  }
  return ok;
}

bool sendDirectMsg(int contactIdx, const char* text) {
  if (!s_ready || !text || !text[0]) return false;
  spiLock();
  int slot = s_mesh->sendDirect(contactIdx, text);
  spiUnlock();
  return slot >= 0;
}

void sendAdvert() {
  if (!s_ready) return;
  spiLock();
  s_mesh->sendSelfAdvertNow();
  spiUnlock();
}

int joinHashChannel(const char* name) {
  if (!s_ready || !name || !name[0]) return -1;
  spiLock();
  int idx = s_mesh->joinHash(name);
  spiUnlock();
  return idx;
}

bool sendHashChannelMsg(const char* name, const char* text) {
  if (!s_ready || !text || !text[0]) return false;
  int idx = joinHashChannel(name);
  if (idx < 0) return false;
  spiLock();
  bool ok = s_mesh->sendToChannel(idx, text);
  spiUnlock();
  if (ok) {
    MsgView m{};
    m.kind = 0;
    snprintf(m.text, sizeof(m.text), "[%s%s] %s: %s",
             (name[0] == '#') ? "" : "#", name, s_mesh->name(), text);
    m.timestamp = 0;
    m.contactIdx = 0xFF;
    m.channelIdx = (uint8_t)idx;
    pushMsg(m);
  }
  return ok;
}

int channelCount() {
  if (!s_ready) return 0;
  // getChannel liefert auch leere Slots (bis MAX_GROUP_CHANNELS) — Kanäle
  // werden sequentiell angelegt, also zählt bis zum ersten leeren Namen.
  ChannelDetails cd;
  int n = 0;
  while (s_mesh->getChannelInfo(n, cd) && cd.name[0]) n++;
  return n;
}

bool channelName(int i, char* out, size_t n) {
  if (!s_ready) return false;
  ChannelDetails cd;
  if (!s_mesh->getChannelInfo(i, cd) || !cd.name[0]) return false;
  strncpy(out, cd.name, n - 1);
  out[n - 1] = '\0';
  return true;
}

int contactCount() { return s_ready ? s_mesh->getNumContacts() : 0; }

bool contactName(int i, char* out, size_t n) {
  if (!s_ready) return false;
  ContactInfo c;
  if (!s_mesh->getContactByIdx(i, c)) return false;
  strncpy(out, c.name, n - 1);
  out[n - 1] = '\0';
  return true;
}

const char* nodeName() { return s_ready ? s_mesh->name() : "-"; }

void setNodeName(const char* name) {
  if (!name || !name[0]) return;
  settings::setMeshName(name);
  if (s_ready) s_mesh->setName(name);
}

void applyRadioParams() {
  if (!s_ready || !s_radio) return;
  settings::MeshParams p = settings::meshParams();
  spiLock();
  s_radio->setFrequency(p.freqMhz);
  s_radio->setBandwidth(p.bwKhz);
  s_radio->setSpreadingFactor(p.sf);
  s_radio->setCodingRate(p.cr);
  // Längere Präambel bei niedrigem SF (kürzere Symbole) — vgl. MeshCore PR#1954.
  s_radio->setPreambleLength((p.sf <= 8) ? 32 : 16);
  s_radio->setOutputPower(p.txDbm);
  // Zurück in den Empfangsmodus (Parameterwechsel beendet laufendes RX).
  s_radio->startReceive();
  spiUnlock();
}

uint32_t rtcTime() {
  return s_ready ? s_rtc.getCurrentTime() : 0;
}

bool radioStats(int* noiseFloor, uint32_t* rxPkts, uint32_t* txPkts) {
  if (!s_ready || !s_radioDrv) return false;
  if (noiseFloor) *noiseFloor = s_radioDrv->getNoiseFloor();
  if (rxPkts)     *rxPkts     = s_radioDrv->getPacketsRecv();
  if (txPkts)     *txPkts     = s_radioDrv->getPacketsSent();
  return true;
}

}  // namespace mesh_client
