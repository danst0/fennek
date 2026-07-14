# CLAUDE.md

## Projekt

**Fennek** — Multi-App-Handheld-Firmware für das **LilyGO T-Deck Pro V1.1** (ESP32-S3, 8 MB PSRAM), PlatformIO/Arduino. Remote `danst0/fennek`. Version: `FENNEK_VERSION` in `src/config.h`, Log-Präfix `[FENNEK]`, NVS-Namespace `fennek`, SD-Cache `/.fennek`. Apps: Musik, Hörbuch, Lesen, Mesh, Spiele, Dateien, Notizen, Wecker, Karten, Kopfrechnen, Karteikarten, Kalender, Todo. Code/Doku/Serial-Ausgaben **deutsch**; Textausgaben über `gui::print` (UTF-8 → CP437).

- Aktiver Code `src/`, einzige Build-Env `fennek`. `lib/meshcore/` + `lib/ed25519/` = vendored MeshCore. Alte Meck-Firmware in der Git-Historie.
- `boards/t-deck_pro.json`, Partition `default_16MB.csv` (6,5 MB App-Slots, 3,4 MB SPIFFS, 20 KB NVS).

## Build & Flash

```bash
pio run -e fennek              # bauen
pio run -e fennek -t upload    # flashen (/dev/ttyACM0, 921600 Baud)
pio device monitor -b 115200  # Boot-Log + Exception-Decoder
```

Keine Unit-Tests; Verifikation am Gerät. Bootet vollständig **ohne SD-Karte**.

Debug-Flags (`-D`): `AUDIO_DEBUG_GAP`, `PLAYLIST_SELFTEST`, `MESH_SMOKE_TEST`, `GAMES_SMOKE_TEST`, `SLEEP_WAKE_TEST`, `BATTLOG`.

## Toolchain — nicht aktualisieren

`espressif32@6.11.0` (Arduino-ESP32 2.0.17 / IDF 4.4), `ESP32-audioI2S#2.0.6`, RadioLib ^7.3 (`RADIOLIB_GODMODE=1`), rweather/Crypto, densaugeo/base64. Upgrades nur auf ausdrücklichen Wunsch.

## Architektur — Anti-Stotter-Invarianten

**E-Ink, SD und LoRa-SX1262 teilen denselben SPI-Bus** (HSPI, SCLK 36/MOSI 33/MISO 47).

```
Core 0:  Audio-Decode-Task (Prio 4) ── audio.loop()
Core 1:  Arduino loop() ── appmgr ── g_spiMutex (core/board.h)
Core 1:  Mesh-Pumpe (mesh_app::background → spiLock)
```

1. **Jeder SPI-Zugriff (SD, E-Ink, LoRa) unter `spiLock()`/`spiUnlock()`.**
2. **`audio.*`-Aufrufe nur im Audio-Task** — UI sendet Kommandos via Queue (`services/audio.cpp`).
3. **SPI-Mutex-Freigabe während E-Ink-BUSY** (`setBusyCallback` in `core/display.cpp`).
4. **256 KB PSRAM-Read-Ahead** (`audio.setBufsize(-1, 256*1024)`).
5. **SPI auf 4 MHz** (`SPI_BUS_HZ`) — 8 MHz korrumpiert SD-Writes.
6. **LoRa-CS (GPIO 3) im Leerlauf HIGH**; Radio nutzt bestehende `g_spi`-Instanz (`P_LORA_SCLK` NICHT definieren).
7. **E-Ink-Refresh nur bei Änderung** (Dirty-Flag); Fortschritt/Statuszeile nur als `renderRegion`.
8. **NVS für Settings/Bookmarks** (nie SPI-Bus). SD für Bulk-Caches + Mesh-Daten. Mesh-Identity auf SPIFFS.
9. **WiFi aktiv ⇒ `audio::stop()` + `mesh_client::setSuspended(true)`** (webfm-Regel — alle WLAN-Dienste).

## Module (`src/`)

- `config.h` — Pins & Konstanten.
- `core/board.*` — SPI-Bus, SD-Mount, `g_spiMutex`, `loraPower`, `gpsPower`. GPS (GPIO39) + DRV2605 (GPIO2) beim Boot aus. Kein 4G-Modem (diese Variante = Audio, PCM5102A-DAC).
- `core/display.*` — GxEPD2-E-Ink, `render(fn,full)` / `renderRegion(fn,y,h)`, BUSY-Callback.
- `core/gui.*` — `toCp437`/`print`/`printAt`/`textBounds`, `drawButton`, `drawRowText`, `Rect`.
- `core/touch.*` + `core/hyn/` — CST328 (I2C 13/14). **hyn/ nicht refactoren.**
- `core/keyboard.*` — TCA8418 (I2C 0x34), Sticky Shift/Alt/Sym, Key-Repeat 400/150 ms.
- `core/battery.*` — BQ27220-Fuel-Gauge (nur Lesen).
- `core/settings.*` — NVS: Lautstärke, letzte App, Resume-Positionen, Bookmarks.
- `core/console.*` — USB-Debug-Konsole (`help`). Befehle: `status`, `time`/`tz`, `mesh init`, `advert [flood]`, `public`/`join`/`chan`/`dm`, `pos`, `gps [scan|off]`, `wifi`, `alarm`, `ollama`, `podcast`, `ota`, `todo`, `cal`, `nav`, `scrobble`, `notes`, `rm`. Konsolen-Eingabe verlängert Auto-Standby (`power::noteActivity()`).
- `core/power.*` — Knopf (kurz=Tastensperre, lang=Standby), Auto-Standby, Deep Sleep. **Zwei Seitenknöpfe: OBEN=GPIO0 (Firmware), UNTEN=RST (Hardware-Reset — sofort, Firmware sieht nichts).** Timer-Wake: Minimal-Pfad (nur Banner), alle 12 Wakes Vollbild. Wecker-Wake: `handleTimerWake()` gibt `false` → Vollboot. Schutznetze (nach 3 stillen Tiefentladungen 06-07/26): Task-WDT um Minimal-Pfad + `enterStandby()`, Timeout der Loslass-Warteschleife, `noteBoot()`-Crash-Loop-Bremse (3 abnormale Resets → Not-Standby nur Knopf-Wake), Tiefentladeschutz (≤5 % → Standby ohne Stunden-Wake), Schlaf-Diagnose im RTC-RAM (`logSleepDebug()` kippt Phase + Wake-Historie ins battlog).
- `core/appmgr.*` — App-Lifecycle, Statuszeile, Dirty-Koaleszenz, 30-s-Persistenz. Tap Statuszeile = Home.
- `services/timesync.*` — Kein HW-RTC; kanonische Uhr = ESP32-Systemzeit. Quellen (Prio): GPS > NTP > Mesh-Adverts. Drift-Lernen, Pre-Standby-Sync, Zeitzone als POSIX-TZ (NVS `tz`, Default Europe/Berlin).
- `services/audio.*` — Pfadbasierte Queue (PSRAM-Staging), Owner-Token (Music/Book/Podcast), Shuffle/Repeat, `seekRel`, Sleep-Timer.
- `services/scrobble.*` — Navidrome/Subsonic. WLAN⊥Audio → Batch vor Auto-Standby. Queue → `/.fennek/scrobbles.tsv`. NVS `nscro/nurl/nuser/npass`.
- `services/notes_ai.*` — Ollama-Schönschreiben vergangener Notizen. Batch vor Auto-Standby, max. 20/Lauf. Done-Liste `/.fennek/notes_ai.done` (SHA256). NVS `oai/ourl/omod`.
- `services/ota.*` — OTA via GitHub-Releases-API (`danst0/fennek`). Löst 302-Redirect selbst auf. Flasht in inaktiven OTA-Slot. NVS `otau`. Release: `gh release create vX.Y.Z .pio/build/fennek/firmware.bin`.
- `services/podcast.*` — **DEAKTIVIERT (v2.5.9)**. Code bleibt für Reaktivierung. Abos `/podcasts/feeds.txt`, je Feed nur neueste Folge.
- `services/library.*` — Musik `/music`, PSRAM-Blöcke (kein Track-Limit), Cache `/.fennek/tracks.bin`. `TRACK_PATH_LEN`=256 (längere überspringen).
- `services/id3.*` — Minimaler ID3v1/v2-Reader (TIT2/TPE1/TALB).
- `services/gps.*` — u-blox MIA-M10Q, UART1 38400 Baud. Parst GGA/RMC. UBX-Aiding (Zeit + Grob-Pos) beim Start. GPS nur aktiv solange Maps-App vorn.
- `services/maps_tiles.*` — 1-bit-Kacheln `/maps/{z}/{x}/{y}.bin` (8192 B). PSRAM-LRU-Cache (16 Slots). `ensureViewport()` nur aus `tick()`.
- `services/textdoc.*` — Streaming-Paginierung, Offset-Index-Cache `/.fennek/idx/`.
- `services/epubzip.h`/`epubproc.*` — EPUB→TXT, Cache `/books/.epub_cache/`.
- `services/alarmclock.*` — 4 Wecker (NVS `alarm`), Schlummer (RTC-RAM). Piepton `/.fennek/alarm.wav`, Backlight-Blinken (GPIO42). Signal-Modus pro Wecker (Ton/Blinken/Beides). Auto-Quittung 5 min.
- `services/calendar.*` + `apps/ical_core.h` — iCal-Abo, read-only. Feeds `/calendar/feeds.txt`. Bedingter GET (ETag/304), Streaming-Parser, Fenster jetzt−1…+56 Tage. RRULE DAILY/WEEKLY/MONTHLY. NVS-Toggle `calauto`.
- `services/reinschrift.*` + `apps/reinschrift_core.h` — Todo-Sync via WebDAV (Nextcloud). Op-Queue `/.fennek/todo_ops.tsv` + ETag-gestützter PUT mit Konflikt-Merge (412→GET+Merge). NVS `todourl/todousr/todopw/todopath`.
- `services/webfm.*` — WLAN-Dateiserver (`http://fennek.local`), JSON-API + OTA-Endpunkte. SD-Zugriffe unter spiLock, Netz-I/O nach spiUnlock.
- `apps/launcher.*` — 2 Seiten à 10 Kacheln. S1: Musik/Hörbuch/Lesen/Mesh/Optionen/Spiele/Dateien/Notizen/Wecker/Karten. S2: Lage/Rechnen/Lernen/Kalender/Todo.
- `apps/music_app.*` — Künstler/Album/Playlist-Browser + Player.
- `apps/book_app.*` — Hörbücher `/audiobooks`, NVS-Bookmarks, ±30 s.
- `apps/reader_app.*` — Bücher `/books` (.txt/.epub), Fortschrittsanzeige.
- `apps/mesh_client.*` — `MeshClient : BaseChatMesh`. SD-Persistenz: `/meshcore/messages.log` (256-KB-Rotation), `/meshcore/contacts.bin`, `/meshcore/channels.txt`. SD-Writes deferred (5-s-Drossel, nie aus Mesh-Callback).
- `apps/mesh_app.*` — Kanal-/Kontakt-/DM-Screens. Radio-Init lazy beim ersten Betreten.
- `apps/games_app.*` — 2048, Minensucher, Schach (Negamax+AB, FreeRTOS-Task Prio 1), TTT, Sudoku (iterativer Generator, rekursionsfrei). Logik in Arduino-freien `*_core.h`, host-getestet.
- `apps/files_app.*` — WebFM-Start/Stop, SSID/IP-Anzeige.
- `apps/notes_app.*` — Tagesnotizen `/notes/YYYY-MM-DD.md`.
- `apps/alarms_app.*` — Wecker-UI, Ziffern-Direkteingabe, Klingel-Screen.
- `apps/maps_app.*` — Karten + GPS-Marker. Tile-Load in `tick()`, Blit in `draw()`. Follow ab >24 px Bewegung. Mesh-Pos-Nachführung (≥30 s & >10 m).
- `apps/mathquiz_app.*` — Kopfrechnen. CFO-Modus (SHARE/GROWTH/MARKUP/DISCOUNT/PCT/RULE72). Adaptive Schwierigkeit (Skill-Zähler, je Modus persistiert, NVS `mqlvl`). **Zeit-Ziel ~20 s/Aufgabe:** Zahlenbereiche gedeckelt + Schätz-Toleranz (`Problem.tol`/`toleranceFor`/`accept` in `mathquiz_core.h`) — bei schätzbaren Aufgaben (große Produkte ab Mittel, Geld-Prozente, SHARE/GROWTH) zählt eine gute Näherung als richtig („Gut geschätzt!", exakter Wert wird gezeigt); Plus/Minus/glatte Division/Rule72 bleiben exakt. Feedback nennt die gebrauchten Sekunden.
- `apps/flashcards_app.*` — Leitner-Boxen 0–4, Decks `/flashcards/*.txt`, Fortschritt `/flashcards/.progress/`.
- `tools/host_test_games.cpp` / `host_test_apps.cpp` — Host-Tests der Arduino-freien Cores.
- `main.cpp` — Init: `handleTimerWake()` → Power → Display → Touch/KB/Akku/Settings → SD/Library → Audio → Apps.

## Funkparameter

Default **„EU/UK Narrow"**: **869,618 MHz, BW 62,5 kHz, SF 8, CR 4/5**, 22 dBm, DIO2-RF-Switch, TCXO 2,4 V. Maßgeblich sind die NVS-Werte (`settings::meshParams()`), Settings-App „Optionen → Funk". `initContacts()` **muss** in `MeshClient::begin()` gerufen werden — sonst NULL-`contacts`-Crash.

## Bekannte Fallen (nicht reintroduzieren)

1. **`initContacts()` in `MeshClient::begin()`** — fehlt → NULL-Crash beim ersten Advert.
2. **Kein `spiLock()` aus Code, der schon unter dem Lock läuft** — nicht-rekursiver Mutex → komplettes Einfrieren (Loop, Touch, KB, Serial, kein Watchdog). Merke: `MeshClient::begin()`, alle Mesh-Callbacks und `App::draw()` laufen bereits unter spiLock.
3. **Uhr-Sync: nicht blind vorwärts ratschen.** Regeln: Vorwärts <1 h, Rückwärts erst nach 3 Adverts >1 h hinter uns. `s_clockConfident`-Bootstrap für ersten plausiblen Sprung nach Boot. ESP32-Systemzeit übersteht Deep Sleep; `millis()` nicht.
4. **Kontakt-Tabelle voll (MAX_CONTACTS=64):** `shouldOverwriteWhenFull() = true` — ältester Nicht-Favorit wird ersetzt.
5. **Auto-Standby kennt Konsole:** `power::noteActivity()` in `console::handleLine()`.
6. **Standby-Tests auf Akku:** USB-JTAG-DTR kann GPIO0-Druck als Reset interpretieren.
7. **Flood statt Zero-Hop für Mesh-Position:** Zero-Hop erreicht 1-Hop-Bridges nicht.
8. **GPS-Baudrate 38400** (MIA-M10Q) — bei 9600 nur Müll.
