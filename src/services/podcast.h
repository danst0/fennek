// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// podcast.h — Podcast-Abonnements: Feed-Verwaltung + Episoden-Sync.
//
// Hintergrund (CLAUDE.md): WLAN und Audio schließen sich auf dieser Hardware
// aus (WiFi-Task auf Core 0 verdrängt den Audio-Task, beide am SPI-Bus). Der
// Sync verwaltet das WLAN daher selbst (audio::stop + mesh suspend), lädt die
// Episode gechunkt auf die SD (Netz-I/O nach spiUnlock, SD-Writes unter
// spiLock) und schaltet das WLAN wieder ab — wie scrobble/notes_ai.
//
// Speichermodell (alles auf SD, ohne SD wird still übersprungen):
//   /podcasts/feeds.txt        — Abos, eine Zeile "URL" oder "URL<TAB>Name".
//                                Per WebFM editierbar (wie /meshcore/channels.txt).
//   /podcasts/<slug>/          — ein Ordner je Feed (Slug aus der URL).
//   /podcasts/<slug>/episode.* — die EINE behaltene (neueste) Folge.
//   /podcasts/<slug>/state.txt — guid/title/file der lokal vorliegenden Folge.
//
// Default-Policy: "immer nur die letzte Folge behalten" — beim Sync wird das
// erste <item> des Feeds geladen und jede ältere Audiodatei im Ordner gelöscht.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace podcast {

constexpr int kMaxFeeds = 16;

struct Feed {
  char url[256];
  char name[64];    // Anzeigename (aus feeds.txt; sonst Slug)
  char slug[40];    // Ordnername unter /podcasts
};

// Lokal vorliegende Episode eines Feeds (aus state.txt).
struct Local {
  char title[96];
  char guid[96];
  char file[128];   // voller SD-Pfad der Audiodatei ("" = keine)
};

// Standard-Feed, mit dem feeds.txt beim ersten Start vorbelegt wird.
constexpr const char* kDefaultFeedUrl  = "https://freakshow.fm/feed/mp3/";
constexpr const char* kDefaultFeedName = "Freakshow";

// /podcasts + feeds.txt sicherstellen (legt den Default an, falls leer).
// SPI-leichtgewichtig, aus main() nach SD-Mount aufrufbar.
void begin();

// --- Feed-Verwaltung (liest/schreibt feeds.txt unter spiLock) ----------------
int  readFeeds(Feed* out, int cap);
int  feedCount();
bool feed(int idx, Feed* out);
bool addFeed(const char* url, const char* name);   // hängt an feeds.txt an
bool removeFeed(int idx);

// Lokal vorliegende Episode (aus <slug>/state.txt). false = noch keine.
bool localEpisode(const Feed& f, Local* out);

// Fortschritts-Callback für die UI (phase: kurzer Text; pct -1 = unbestimmt).
typedef void (*Progress)(const char* phase, int pct);

// Manueller Sync (Konsole/App): verwaltet WLAN selbst, blockierend. Lädt für
// jeden Feed die neueste Folge, falls neu, und löscht ältere. log bekommt eine
// kurze Statuszeile. true = ≥1 neue Folge geladen oder nichts zu tun.
bool syncAll(char* log, size_t logN, Progress prog);

// Wie syncAll, aber nur ein Feed (idx).
bool syncOne(int idx, char* log, size_t logN, Progress prog);

// Auto-Sync VOR dem Auto-Standby (RTC-RAM-Back-off; nur wenn
// settings::podcastAutoSync()). Synchronisiert je Standby NUR EINEN Feed
// (Round-Robin), damit das WLAN kurz bleibt. No-op ohne Feeds/Konfiguration.
bool flushBeforeStandby();

}  // namespace podcast
