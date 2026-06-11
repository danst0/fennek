// =============================================================================
// settings.h — Persistenz über NVS (Preferences).
//
// NVS liegt im internen QSPI-Flash: Writes berühren den geteilten HSPI-Bus
// NIE — keine Mutex-Konkurrenz mit Audio-Streaming oder E-Ink-Refresh,
// atomar und wear-leveled. Schreibdisziplin: nur bei Änderung schreiben.
//
// Bulk-Daten (ID3-Cache, Seiten-Indizes) gehören NICHT hierher, sondern auf
// die SD-Karte (20-KB-NVS-Partition).
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace settings {

void begin();

// --- Allgemein (Namespace "meck") -------------------------------------------
uint8_t volume();
void    setVolume(uint8_t v);          // schreibt nur bei Änderung

void    lastApp(char* out, size_t n);  // Name der zuletzt aktiven App ("" = keine)
void    setLastApp(const char* name);

// --- Musik: zuletzt gespielter Track (für Resume) ----------------------------
// Pfad + Position; leerer Pfad = nichts gespeichert.
void    lastTrack(char* pathOut, size_t n, uint32_t* posSec);
void    setLastTrack(const char* path, uint32_t posSec);
void    clearLastTrack();

// --- Hörbuch-Bookmarks (Namespace "abook", Key = CRC32 des Buchordners) ------
bool    bookBookmark(uint32_t key, uint16_t* fileIdx, uint32_t* posSec);
void    setBookBookmark(uint32_t key, uint16_t fileIdx, uint32_t posSec);
void    clearBookBookmark(uint32_t key);

// --- eBook-Lesepositionen (Namespace "ebook", Key = CRC32 des Pfads) ---------
bool    readPos(uint32_t key, uint32_t* page);
void    setReadPos(uint32_t key, uint32_t page);

// --- Mesh-Funkparameter --------------------------------------------------------
// Defaults = "EU/UK Narrow" (Standard in Deutschland/NRW, Stand 2026):
// 869,618 MHz, BW 62,5 kHz, SF 8, CR 4/8, 22 dBm. Änderungen werden von
// mesh_client::applyRadioParams() live aufs Radio angewandt.
struct MeshParams {
  float   freqMhz;
  float   bwKhz;
  uint8_t sf;        // 7..12
  uint8_t cr;        // 5..8 (4/x)
  uint8_t txDbm;     // 1..22
};
MeshParams meshParams();
void       setMeshParams(const MeshParams& p);   // schreibt nur Änderungen

void meshName(char* out, size_t n);              // Node-Name (Default "T-Deck")
void setMeshName(const char* name);

// CRC32-Helfer (Cache-/Bookmark-Keys).
uint32_t crc32(const char* s);

}  // namespace settings
