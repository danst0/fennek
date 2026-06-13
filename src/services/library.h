// =============================================================================
// library.h — Track-Liste (MP3-Dateien eines SD-Verzeichnisses).
//
// Wird vom UI-Thread aufgebaut (SD-Scan) und vom Audio-Task gelesen
// (Next/Prev/EOF-Advance). Zugriff ist über einen Mutex serialisiert; der
// Scan läuft unter dem SPI-Mutex (geteilter Bus).
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace library {

void begin();

// --- Aufbau der Bibliothek ----------------------------------------------------
// Normaler Weg beim Boot: loadCache() lädt /.fennek/tracks.bin (Sekunden);
// ohne Cache startScan() + scanStep()-Häppchen aus dem appmgr-Loop (readdir,
// UI bleibt bedienbar). Der Abschluss publiziert die Funde, wendet den
// ID3-Cache an und schreibt den Track-Cache. Fallback auf "/" passiert
// automatisch, wenn unter dem Startverzeichnis nichts gefunden wird.
bool loadCache();
void startScan(const char* dir);
bool scanning();
int  scanFound();                 // bisher gefundene Tracks (nur w. Scan läuft)
void scanStep(int maxEntries);

// Blockierender Komplett-Scan (Selbsttests). Gibt die Track-Anzahl zurück.
int scan(const char* dir);

int count();

// Anzeigename (Dateiname) des i-ten Tracks in out kopieren.
bool name(int i, char* out, size_t outLen);

// Vollständigen SD-Pfad des i-ten Tracks in out kopieren.
bool path(int i, char* out, size_t outLen);

// Künstler/Album des i-ten Tracks (ID3-Tag oder Ordner-Fallback, UTF-8).
bool trackArtist(int i, char* out, size_t outLen);
bool trackAlbum(int i, char* out, size_t outLen);

// Flat-Index zu einem SD-Pfad (-1 = nicht in der Bibliothek).
int indexOfPath(const char* p);

// --- ID3-Tag-Scan (inkrementell) ---------------------------------------------
// scan() wendet zuerst den Cache (/.fennek/id3.bin) an; Dateien ohne Cache-Treffer
// werden danach häppchenweise über tagScanStep() geparst (Aufruf aus dem
// appmgr-Loop), damit die UI bedienbar bleibt. Nach dem letzten Schritt werden
// die Indizes neu sortiert und der Cache zurückgeschrieben.
bool tagScanPending();
void tagScanStep(int maxFiles);
void tagScanProgress(int* done, int* total);

// --- Hierarchie-Queries (Künstler/Album/Titel) -----------------------------
// Künstler/Album werden aus der Ordnerstruktur abgeleitet:
//   <root>/<Künstler>/<Album>/<Titel>.mp3
// Die flache Track-Liste ist nach (Künstler, Album, Name) sortiert; daraus
// sind Künstler- und Album-„Runs" (zusammenhängende Bereiche) vorberechnet.

// Künstler (alphabetisch).
int  artistCount();
void artistName(int artist, char* out, size_t outLen);

// Alben eines Künstlers -> liefert globale Album-Run-IDs.
int  artistAlbumCount(int artist);
int  artistAlbumRun(int artist, int j);

// Alle Alben (nach Albumname sortiert) -> globale Album-Run-IDs.
int  albumCount();
int  albumRunByName(int j);

// Album-Run (ein konkretes Album).
void albumLabel(int run, bool withArtist, char* out, size_t outLen);
int  albumTrackCount(int run);
void albumTrackName(int run, int k, char* out, size_t outLen);
int  albumTrackFlatIndex(int run, int k);     // -> Index für audio::playIndex

// Alle Titel (nach Titelname sortiert).
int  titleCount();
void titleName(int i, char* out, size_t outLen);
int  titleFlatIndex(int i);                   // -> Index für audio::playIndex

// --- Echte .m3u/.m3u8-Playlists --------------------------------------------
// Beim Scan gefundene Playlist-Dateien. Beim Öffnen wird die m3u geparst und
// jeder Eintrag (relativ zum Playlist-Ordner oder absolut) auf einen Track der
// Bibliothek aufgelöst (Pfad-, sonst Dateinamen-Abgleich).
int  playlistCount();
void playlistName(int i, char* out, size_t outLen);   // Dateiname der m3u
int  playlistOpen(int i);                              // parst, cached; #aufgelöste Tracks
void playlistTrackName(int k, char* out, size_t outLen);
int  playlistTrackFlatIndex(int k);                   // -> Index für audio

// Sammelt die Flat-Indizes (in Reihenfolge) für eine Abspiel-Queue.
// scope: art-/album-/title-/playlist-Funktionen liefern Indizes einzeln; hier
// ein Helfer, der alle Flat-Indizes eines Album-Runs in out schreibt.
int  albumTrackFlatList(int run, int* out, int maxN);

}  // namespace library
