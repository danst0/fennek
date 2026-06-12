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
#include "services/library.h"
#include "services/audio.h"
#include "apps/launcher.h"
#include "apps/music_app.h"
#include "apps/book_app.h"
#include "apps/reader_app.h"
#include "apps/mesh_app.h"
#include "apps/mesh_client.h"
#include "apps/settings_app.h"
#include "apps/games_app.h"

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
  // USB-CDC kurz Zeit zum Enumerieren geben (sonst geht das Boot-Log verloren).
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);
  Serial.println("\n[FENNEK] Boot — Fennek " FENNEK_VERSION " (T-Deck Pro)");

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
  }
  library::begin();
  int n = 0;
  if (sdOk) {
    n = library::scan(MP3_DIR);
    if (n == 0) n = library::scan("/");   // Fallback: Root durchsuchen
  }
  Serial.printf("[FENNEK] Tracks gefunden: %d\n", n);

  // 5) Audio-Engine (startet den Decode-Task auf Core 0).
  audio::begin();
  if (settings::volume() != 255) audio::setVolume(settings::volume());
  Serial.printf("[FENNEK] Audio-Engine gestartet (freier PSRAM: %u KB)\n",
                (unsigned)(ESP.getFreePsram() / 1024));

  Serial.printf("[FENNEK] Künstler=%d Alben=%d Titel=%d Playlists=%d\n",
                library::artistCount(), library::albumCount(),
                library::titleCount(), library::playlistCount());

#ifdef PLAYLIST_SELFTEST
  // TEMP: Test-.m3u schreiben (1x absolut, 1x relativ), scannen, auflösen, löschen.
  if (sdOk && library::count() >= 2) {
    char p0[192], p1[192];
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
  launcher::setTile(0, "Musik",    music_app::get());
  launcher::setTile(1, "Hörbuch",  book_app::get());
  launcher::setTile(2, "Lesen",    reader_app::get());
  launcher::setTile(3, "Mesh",     mesh_app::get());
  launcher::setTile(4, "Optionen", settings_app::get());
  launcher::setTile(5, "Spiele",   games_app::get());
  appmgr::begin();
  Serial.println("[FENNEK] Setup fertig — Launcher läuft.");

#ifdef GAMES_SMOKE_TEST
  // TEMP: alle vier Spiele einmal durchklicken (Draw-Pfade + Schach-KI-Task).
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
    char p0[192];
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
  appmgr::loop();
  delay(10);   // ~100 Hz Eingabe-Polling; gibt Core 1 frei
}
