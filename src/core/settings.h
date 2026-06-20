// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

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

// --- Allgemein (Namespace "fennek"; migriert einmalig vom alten "meck") ------
uint8_t volume();
void    setVolume(uint8_t v);          // schreibt nur bei Änderung

void    lastApp(char* out, size_t n);  // Name der zuletzt aktiven App ("" = keine)
void    setLastApp(const char* name);

// Auto-Standby nach Inaktivität (Minuten; 0 = aus). Default 5.
uint8_t standbyMinutes();
void    setStandbyMinutes(uint8_t m);  // schreibt nur bei Änderung

// UI-Sprache (Index in i18n::Lang; 0 = Deutsch). Wird in i18n::tr() gelesen.
uint8_t language();
void    setLanguage(uint8_t lang);     // schreibt nur bei Änderung

// Schriftgröße fürs Lesen (eBooks/TXT) und den Notizen-Editor: 1 = klein
// (Default), 2 = groß. Klassischer 6x8-Font, ganzzahlige Skalierung.
uint8_t fontScale();
void    setFontScale(uint8_t s);       // 1..2; schreibt nur bei Änderung

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

// Zuletzt geöffnetes Buch (kompletter Pfad, "" = keins). Der Reader springt
// beim ersten Betreten nach dem Boot direkt hinein (Seite via readPos).
void    lastBook(char* out, size_t n);
void    setLastBook(const char* path);

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

// Node-Position (Dezimalgrad) für Standortbaken im Advert. 0/0 = nicht gesetzt
// (gilt als unbestimmt — wird dann nicht im Advert mitgesendet).
void meshPos(double* lat, double* lon);
void setMeshPos(double lat, double lon);          // schreibt nur Änderungen

// --- WLAN-Zugangsdaten (Web-Dateiverwaltung; Eingabe via Serial-Konsole) -----
void wifiSsid(char* out, size_t n);              // "" = nicht konfiguriert
void setWifiSsid(const char* ssid);
void wifiPass(char* out, size_t n);
void setWifiPass(const char* pass);

// --- Navidrome/Subsonic-Scrobbling (services/scrobble) ------------------------
// Server-URL inkl. Schema (http/https), Benutzer + Passwort (Klartext, wie WLAN).
// Toggle aktiviert das Erfassen + den Pre-Standby-Upload gespielter Tracks.
bool navEnabled();                               // Default false
void setNavEnabled(bool on);
void navUrl(char* out, size_t n);                // "" = nicht konfiguriert
void setNavUrl(const char* url);
void navUser(char* out, size_t n);
void setNavUser(const char* user);
void navPass(char* out, size_t n);
void setNavPass(const char* pass);

// --- Ollama-KI für Notizen (services/notes_ai) --------------------------------
// Server-URL inkl. Schema+Port (z. B. http://192.168.1.10:11434) und Modellname
// (z. B. "llama3.2"). Toggle aktiviert das automatische "Schoenschreiben" der
// Notizen vor dem Auto-Standby (wenn WLAN frei ist).
bool aiEnabled();                                // Default false
void setAiEnabled(bool on);
void aiUrl(char* out, size_t n);                 // "" = nicht konfiguriert
void setAiUrl(const char* url);
void aiModel(char* out, size_t n);               // Default "llama3.2"
void setAiModel(const char* model);

// --- OTA-Firmware-Update (services/ota) ---------------------------------------
// Manifest-URL: GitHub-Releases-API (Default) oder ein selbst gehostetes Manifest.
// Default = neuestes Release von danst0/fennek. "" lässt OTA-Prüfen fehlschlagen.
void otaUrl(char* out, size_t n);
void setOtaUrl(const char* url);

// --- Podcast (services/podcast) -----------------------------------------------
// Toggle für den automatischen Episoden-Sync VOR dem Auto-Standby (wenn WLAN
// frei ist). Der manuelle Sync in der Podcast-App läuft unabhängig davon.
bool podcastAutoSync();                          // Default false
void setPodcastAutoSync(bool on);

// --- Uhrzeit/Zeitzone (services/timesync) -------------------------------------
uint32_t lastTime();                      // 0 = nie gespeichert (Kaltstart-Fallback)
void     setLastTime(uint32_t epoch);
uint16_t clockPpm();                      // gelernte Oszillator-Drift; 0 = unbekannt
void     setClockPpm(uint16_t ppm);
void     tzString(char* out, size_t n);   // POSIX-TZ (Default Europe/Berlin)
void     setTzString(const char* tz);

// --- Spiele: Beststände + Schach-Spielstand (Namespace "fennek") --------------
// Schreibdisziplin: Writes nur am Spielende bzw. beim Verlassen (Schach-Blob).
uint32_t best2048();                       // 0 = noch kein Score
void     setBest2048(uint32_t score);      // schreibt nur bei neuem Rekord
uint16_t minesWins();
uint16_t minesBestSec();                   // 0 = noch kein Sieg
void     setMinesResult(bool won, uint16_t sec);
uint16_t chessWins();                      // Siege gegen das Gerät
void     addChessWin();
bool     chessGame(void* buf, size_t n);   // laufende Partie laden (true = da)
void     setChessGame(const void* buf, size_t n);   // n=0 löscht
uint16_t tttWins();
uint16_t tttDraws();
void     addTttResult(bool win, bool draw);
uint16_t mathBestStreak();                 // Kopfrechnen: längste Antwort-Serie
void     setMathBestStreak(uint16_t streak);   // schreibt nur bei neuem Rekord

// CRC32-Helfer (Cache-/Bookmark-Keys).
uint32_t crc32(const char* s);

// --- INI-Export/Import (SD-Spiegel, services/settingsfile) --------------------
// Bewusst SPI-frei: beide arbeiten nur auf einem RAM-Puffer; die SD-I/O liegt in
// services/settingsfile.* (unter spiLock). exportIni() serialisiert alle Werte,
// importIni() parst sie zurück ins NVS. Pro-Buch-Bookmarks/Lesepositionen sind
// CRC32-verschlüsselt (Pfad nicht rekonstruierbar) und daher NICHT enthalten.
size_t exportIni(char* out, size_t cap);   // gibt geschriebene Länge zurück
int    importIni(const char* text);        // angewandte Schlüssel; -1 = Parse-Fehler

}  // namespace settings
