// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// main.cpp — LilyGO T-Deck Pro: Multi-App-Handheld (Launcher + Apps).
//
// Aufbau:
//   Core 0  : Audio-Decode-Task (services/audio.cpp) — höchste Priorität
//   Core 1  : Arduino loop() — Eingabe-Polling + E-Ink-UI (core/appmgr.cpp)
// Beide teilen sich den SPI-Bus (E-Ink + SD); g_spiMutex serialisiert ihn.
// =============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <SD.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <string.h>

#include "config.h"
#include "core/board.h"
#include "core/display.h"
#include "core/touch.h"
#include "core/keyboard.h"
#include "core/battery.h"
#include "core/settings.h"
#include "core/appmgr.h"
#include "core/console.h"
#include "core/power.h"
#include "services/library.h"
#include "services/audio.h"
#include "services/webfm.h"
#include "services/timesync.h"
#include "services/scrobble.h"
#include "services/podcast.h"
#include "services/alarmclock.h"
#include "services/settingsfile.h"
#include "services/battlog.h"
#include "apps/launcher.h"
#include "apps/music_app.h"
#include "apps/book_app.h"
#include "apps/reader_app.h"
#include "apps/notes_app.h"
#include "apps/mesh_app.h"
#include "apps/mesh_client.h"
#include "apps/settings_app.h"
#include "apps/games_app.h"
#include "apps/files_app.h"
#include "apps/alarms_app.h"
#include "apps/maps_app.h"
#include "apps/gyro_app.h"
#include "apps/mathquiz_app.h"
#include "apps/flashcards_app.h"
#include "apps/podcast_app.h"

#ifdef GAMES_SMOKE_TEST
namespace {
// Synthetische Eingabe an die aktive App + ein Loop-Durchlauf (rendert dirty).
void gamesSmokeKey(char k) {
  InputEvent e; e.type = InputEvent::KEY; e.x = e.y = 0; e.key = k;
  appmgr::current()->handleInput(e);
  appmgr::loop();
}
void gamesSmokeTap(int16_t x, int16_t y) {
  InputEvent e; e.type = InputEvent::TAP; e.x = x; e.y = y; e.key = 0;
  appmgr::current()->handleInput(e);
  appmgr::loop();
}
}  // namespace
#endif

void setup() {
  Serial.begin(115200);

  // Stunden-Timer-Wake aus dem Standby? Dann Minimal-Pfad: nur Akku-% aufs
  // Schlafbild und sofort zurück in Deep Sleep (kehrt dann nicht zurück).
  // Bei Knopf-Wake/Kaltstart geht es hier normal weiter.
  power::handleTimerWake();

  // USB-CDC kurz Zeit zum Enumerieren geben (sonst geht das Boot-Log verloren).
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);
  Serial.println("\n[FENNEK] Boot — Fennek " FENNEK_VERSION " (T-Deck Pro)");
  // Diagnose Standby-Sofort-Wake: Warum sind wir hier? (Reset-Grund + Wake-Quelle)
  Serial.printf("[FENNEK] Reset-Grund=%d Wake-Quelle=%d ext1=0x%llx\n",
                (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause(),
                (unsigned long long)esp_sleep_get_ext1_wakeup_status());

  // 1) Power + Busse.
  board::powerOn();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);
  board::initBus();
  Serial.println("[FENNEK] Power + Busse ok");

  // 2) Display zuerst — Nutzer sieht sofort etwas.
  display::begin();
  Serial.println("[FENNEK] Display init ok");

  // 3) Eingabe + Akku + Settings (alles unabhängig vom SPI-Bus).
  bool touchOk = touch::begin();
  Serial.printf("[FENNEK] Touch init: %s\n", touchOk ? "ok" : "FEHLGESCHLAGEN");
  bool kbOk = keyboard::begin();
  Serial.printf("[FENNEK] Tastatur (TCA8418): %s\n", kbOk ? "ok" : "NICHT gefunden");
  bool battOk = battery::begin();
  Serial.printf("[FENNEK] Akku (BQ27220): %s (%u mV, %u%%)\n",
                battOk ? "ok" : "NICHT gefunden",
                battery::milliVolts(), battery::percent());
  settings::begin();
  // Zeit-Koordinator: Uhr aus NVS wiederherstellen (Kaltstart) + Drift/Zeitzone
  // laden. Muss nach settings::begin() laufen, braucht aber kein SD/Audio.
  timesync::begin();

  // 4) SD + Bibliothek (ohne Karte läuft alles weiter, Apps zeigen Hinweis).
  bool sdOk = board::initSD();
  Serial.printf("[FENNEK] SD-Karte: %s\n", sdOk ? "gemountet" : "NICHT gefunden");
  if (sdOk) {
    // Einmalige Migration: Cache-Verzeichnis hieß früher /.meck (Meck-Ära).
    spiLock();
    if (SD.exists("/.meck") && !SD.exists("/.fennek")) {
      bool ok = SD.rename("/.meck", "/.fennek");
      Serial.printf("[FENNEK] SD-Cache-Migration /.meck -> /.fennek: %s\n", ok ? "ok" : "FEHLER");
    }
    spiUnlock();
    // Einstellungen von der SD ins NVS spiegeln (SD ist beim Boot autoritativ),
    // bevor Audio die Lautstärke und die Apps Mesh-/WLAN-Werte lesen.
    if (settingsfile::importFromSd())
      Serial.println("[FENNEK] Einstellungen aus /fennek.ini übernommen");
  }
  library::begin();
  int n = 0;
  if (sdOk) {
    // Schneller Weg: Track-Cache von der SD; sonst Hintergrund-Scan über die
    // appmgr-Häppchen (Boot blockiert nicht mehr minutenlang — Fallback auf
    // "/" übernimmt die Library selbst, wenn unter /music nichts liegt).
    if (library::loadCache()) n = library::count();
    else                      library::startScan(MP3_DIR);
  }
  Serial.printf("[FENNEK] Tracks gefunden: %d%s\n", n,
                library::scanning() ? " (Scan läuft im Hintergrund)" : "");

  // 5) Audio-Engine (startet den Decode-Task auf Core 0).
  audio::begin();
  if (settings::volume() != 255) audio::setVolume(settings::volume());
  Serial.printf("[FENNEK] Audio-Engine gestartet (freier PSRAM: %u KB)\n",
                (unsigned)(ESP.getFreePsram() / 1024));

  // Scrobble-Queue (offene Einträge von SD laden); braucht SD + settings.
  scrobble::begin();

  // Podcast deaktiviert (WLAN-Sync zu langsam, v2.5.9): /podcasts wird nicht mehr
  // angelegt; App-Kachel + Auto-Sync-Hook sind aus. Code bleibt reaktivierbar.
  // podcast::begin();

  // Wecker-Engine: Wecker aus NVS laden. Klingelt über die Audio-Queue, daher
  // nach audio::begin(); poll() läuft im loop(). Falls dieser Boot ein
  // Wecker-Wake war (power::handleTimerWake → Vollboot), feuert poll() gleich.
  alarmclock::begin();

  // Debug-Akku-Logger (nur mit -D BATTLOG): braucht SD + Akku + Uhr. Die
  // Aufwach-/Boot-Ursache als erste Aktivität festhalten (= „nach Standby").
  BATTLOG_BEGIN();
  BATTLOG_EVENT("Wake", "Reset=%d Wake=%d", (int)esp_reset_reason(),
                (int)esp_sleep_get_wakeup_cause());

  Serial.printf("[FENNEK] Künstler=%d Alben=%d Titel=%d Playlists=%d\n",
                library::artistCount(), library::albumCount(),
                library::titleCount(), library::playlistCount());

#ifdef PLAYLIST_SELFTEST
  // TEMP: Test-.m3u schreiben (1x absolut, 1x relativ), scannen, auflösen, löschen.
  if (sdOk && library::count() >= 2) {
    char p0[TRACK_PATH_LEN], p1[TRACK_PATH_LEN];
    library::path(0, p0, sizeof(p0));
    library::path(1, p1, sizeof(p1));
    const char* rel1 = strstr(p1, "/music/");
    spiLock();
    File w = SD.open("/music/__sel.m3u", FILE_WRITE);
    if (w) {
      w.println("#EXTM3U");
      w.println(p0);                          // absoluter Pfad
      w.println(rel1 ? rel1 + 7 : p1);        // relativ zu /music
      w.close();
    }
    spiUnlock();
    int nn = library::scan(MP3_DIR);
    Serial.printf("[SELF] rescan tracks=%d playlists=%d\n", nn, library::playlistCount());
    for (int i = 0; i < library::playlistCount(); i++) {
      char nm[64]; library::playlistName(i, nm, sizeof(nm));
      int c = library::playlistOpen(i);
      Serial.printf("[SELF] PL '%s' -> %d Tracks\n", nm, c);
      for (int k = 0; k < c; k++) {
        char tn[64]; library::playlistTrackName(k, tn, sizeof(tn));
        Serial.printf("[SELF]    [%d] %s (flat %d)\n", k, tn, library::playlistTrackFlatIndex(k));
      }
    }
    spiLock(); SD.remove("/music/__sel.m3u"); spiUnlock();
    library::scan(MP3_DIR);  // sauberer Endzustand
  }
#endif

  // 6) Apps registrieren: Launcher zuerst (= Home), dann die Kacheln belegen.
  appmgr::add(launcher::get());
  appmgr::add(music_app::get());
  appmgr::add(book_app::get());
  appmgr::add(reader_app::get());
  appmgr::add(mesh_app::get());
  appmgr::add(settings_app::get());
  appmgr::add(games_app::get());
  appmgr::add(files_app::get());
  appmgr::add(notes_app::get());
  appmgr::add(alarms_app::get());
  appmgr::add(maps_app::get());
  appmgr::add(gyro_app::get());
  appmgr::add(mathquiz_app::get());
  appmgr::add(flashcards_app::get());
  // Podcast deaktiviert (WLAN-Sync zu langsam, v2.5.9): nicht registriert/keine Kachel.
  // appmgr::add(podcast_app::get());
  launcher::setTile(0, i18n::Str::TileMusic,    music_app::get());
  launcher::setTile(1, i18n::Str::TileBook,     book_app::get());
  launcher::setTile(2, i18n::Str::TileReader,   reader_app::get());
  launcher::setTile(3, i18n::Str::TileMesh,     mesh_app::get());
  launcher::setTile(4, i18n::Str::TileSettings, settings_app::get());
  launcher::setTile(5, i18n::Str::TileGames,    games_app::get());
  launcher::setTile(6, i18n::Str::TileFiles,    files_app::get());
  launcher::setTile(7, i18n::Str::TileNotes,    notes_app::get());
  launcher::setTile(8, i18n::Str::TileAlarm,    alarms_app::get());
  launcher::setTile(9, i18n::Str::TileMaps,     maps_app::get());
  launcher::setTile(10, i18n::Str::TileGyro,    gyro_app::get());   // Seite 2, Slot 0
  launcher::setTile(11, i18n::Str::TileMath,    mathquiz_app::get());
  launcher::setTile(12, i18n::Str::TileCards,   flashcards_app::get());
  // Podcast deaktiviert (v2.5.9): Slot 13 bleibt leer (Seite 2, Slot 3).
  // launcher::setTile(13, i18n::Str::TilePodcast, podcast_app::get());
  appmgr::begin();
  Serial.println("[FENNEK] Setup fertig — Launcher läuft.");

#ifdef GAMES_SMOKE_TEST
  // TEMP: alle fünf Spiele einmal durchklicken (Draw-Pfade + Schach-KI-Task).
  Serial.println("[GAME] SMOKE: Start");
  appmgr::launch(games_app::get());
  appmgr::loop();                  // Spiele-Menü rendern
  gamesSmokeKey('\r');             // 2048 öffnen (Auswahl 0)
  gamesSmokeKey('a');
  gamesSmokeKey('s');
  gamesSmokeKey('d');
  gamesSmokeKey('w');
  gamesSmokeKey('\b');             // zurück ins Menü
  gamesSmokeKey('s');              // -> Minensucher
  gamesSmokeKey('\r');
  gamesSmokeTap(120, 170);         // mittlere Zelle aufdecken (Flood-Fill)
  gamesSmokeKey('f');              // Flaggen-Modus an
  gamesSmokeTap(12, 64);           // Zelle 0 flaggen
  gamesSmokeKey('f');
  gamesSmokeKey('\b');
  gamesSmokeKey('s');              // -> Schach
  gamesSmokeKey('\r');             // Setup-Dialog
  gamesSmokeKey('s');              // Zeile "Farbe"
  gamesSmokeKey('d');              // -> Schwarz (Fennek = Weiß beginnt)
  gamesSmokeKey('s');
  gamesSmokeKey('s');              // auf "Start"
  gamesSmokeKey('\r');             // Partie startet, KI denkt
  {
    uint32_t t0 = millis();
    while (millis() - t0 < 20000) { appmgr::loop(); delay(10); }
  }
  gamesSmokeKey('\b');             // zurück (sichert Partie ins NVS)
  gamesSmokeKey('s');              // -> Tic-Tac-Toe
  gamesSmokeKey('\r');
  gamesSmokeTap(48, 100);          // Zelle 0 setzen, Fennek antwortet
  gamesSmokeKey('\b');             // zurück ins Menü
  gamesSmokeKey('s');              // -> Sudoku
  gamesSmokeKey('\r');             // generiert ein Rätsel (Solver/Generator)
  gamesSmokeKey('d');              // Cursor bewegen
  gamesSmokeKey('s');
  gamesSmokeKey('5');              // Ziffer setzen (falls keine Vorgabe)
  gamesSmokeKey('0');              // Zelle wieder leeren
  gamesSmokeKey('m');              // Schwierigkeit wechseln -> neu generieren
  gamesSmokeKey('\b');             // zurück ins Menü
  Serial.printf("[GAME] SMOKE: fertig — freier Heap %u KB\n",
                (unsigned)(ESP.getFreeHeap() / 1024));
  appmgr::goHome();
  appmgr::loop();
#endif

#ifdef MESH_SMOKE_TEST
  // TEMP: Radio-Bring-up ohne UI-Interaktion verifizieren (Serial-Log).
  Serial.println("[FENNEK] MESH_SMOKE_TEST: initialisiere Mesh ...");
  bool meshOk = mesh_client::begin();
  Serial.printf("[FENNEK] MESH_SMOKE_TEST: %s (freier Heap: %u KB)\n",
                meshOk ? "OK" : "FEHLGESCHLAGEN",
                (unsigned)(ESP.getFreeHeap() / 1024));
#endif

#ifdef AUDIO_DEBUG_GAP
  // TEMP: Track 0 spielen und alle 2 s einen Refresh erzwingen — der Audio-Task
  // meldet [GAP] maxGap; bleibt der trotz Refresh klein, stottert es nicht.
  if (n > 0) {
    char p0[TRACK_PATH_LEN];
    library::path(0, p0, sizeof(p0));
    audio::queueBegin(audio::Owner::Music);
    audio::queueAdd(p0);
    audio::queueCommit(0);
    for (int i = 0; i < 12; i++) {
      delay(2000);
      Serial.printf("[FENNEK] >>> erzwinge Display-Refresh #%d <<<\n", i + 1);
      display::render([](Adafruit_GFX& g) {
        g.setTextSize(2); g.setCursor(10, 150); g.print("REFRESH-TEST");
      }, false);
    }
  }
#endif
}

void loop() {
  console::poll();   // Serial-Debug-Konsole ('help' über USB)
  webfm::poll();     // WLAN-Statusmaschine + HTTP-Requests (no-op wenn aus)
  timesync::poll();  // Zeit frisch halten (opportunistisch NTP, NVS-Sicherung)
  scrobble::poll();  // Scrobble-Queue gedrosselt nach SD persistieren
  alarmclock::poll();// Wecker: Fälligkeit prüfen + klingeln
  BATTLOG_POLL();    // Debug-Akku-Logger (no-op ohne -D BATTLOG)
  appmgr::loop();

  // Adaptives Eingabe-Polling: frische ~100 Hz, solange etwas passiert
  // (Eingabe, laufende Wiedergabe hält power::idleMs() bei ~0, oder WLAN
  // bedient gerade HTTP-Requests). Nach ein paar Sekunden Leerlauf auf ~30 Hz
  // drosseln — spart Core-1-Wachzeit, ohne dass sich Touch/Tastatur träge
  // anfühlen (Key-Repeat/Debounce sind millis()-basiert; Audio läuft auf
  // Core 0). Greift nur im wachen Leerlauf vor dem Auto-Standby.
  const bool busy = power::idleMs() < 3000 || webfm::state() != webfm::State::OFF;
  delay(busy ? 10 : 33);
}
