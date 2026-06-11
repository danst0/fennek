# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Projekt

**Fennek** — Multi-App-Handheld-Firmware für das **LilyGO T-Deck Pro V1.1** (ESP32-S3, 8 MB PSRAM), PlatformIO/Arduino. (Repo-/Ordnername „Meck“ stammt vom Fork-Ursprung; die Firmware heißt Fennek. Version: `FENNEK_VERSION` in `src/config.h`, Log-Präfix `[FENNEK]`. Interne Identifier wie der NVS-Namespace `meck` und der SD-Cache `/.meck` bleiben aus Kompatibilität unverändert.) Launcher-Homescreen + vier Apps: **Musik** (MP3-Player), **Hörbuch**, **Lesen** (TXT/EPUB) und **Mesh** (schlanker MeshCore-Client). Fokus: flüssige Bedienung (Touch + Tastatur) und stotterfreie Wiedergabe; Hintergrund-Audio läuft über App-Wechsel hinweg. Code-Kommentare, Serial-Ausgaben und Doku sind **deutsch**; alle Textausgaben laufen über `gui::print` (UTF-8 → CP437, Umlaute!).

- Aktiver Code in `src/` (einzige Build-Env: `mp3player`).
- `lib/meshcore/` + `lib/ed25519/` — vendored MeshCore-Stack (Subset aus dem Archiv) für die Mesh-App.
- `archive_legacy/` — alte MeshCore-/Meck-Firmware; wird **nicht** gebaut, dient als Referenz für hardware-verifizierte Pin-/Init-Sequenzen (Quellbasis der Vendor-Kopien).
- `boards/` — relevant ist nur `t-deck_pro.json` (Partition `default_16MB.csv`: 6,5-MB-App-Slots, 3,4 MB SPIFFS, 20 KB NVS).

## Build & Flash

```bash
pio run -e mp3player              # bauen
pio run -e mp3player -t upload    # flashen (USB-C an /dev/ttyACM0, 921600 Baud)
pio device monitor -b 115200      # serielles Boot-Log (mit Exception-Decoder)
```

Keine Unit-Tests/Lint; Verifikation am Gerät über das Boot-Log (`[MECK] …`/`[MESH] …`-Zeilen). Die Firmware bootet und läuft vollständig **ohne SD-Karte** (Apps zeigen „Keine SD-Karte“ + „Erneut suchen“).

Debug-Flags (als `-D` ergänzen):
- `AUDIO_DEBUG_GAP` — Stotter-Test: loggt `[GAP] … maxGap=…ms`; muss unter der I2S-DMA-Tiefe (~90 ms) bleiben.
- `PLAYLIST_SELFTEST` — schreibt/scannt/löscht eine Test-`.m3u` beim Boot.
- `MESH_SMOKE_TEST` — initialisiert das Mesh-Radio beim Boot (Verifikation ohne UI).

## Toolchain ist bewusst gepinnt — nicht aktualisieren

`espressif32@6.11.0` (Arduino-ESP32 2.0.17 / IDF 4.4) und `ESP32-audioI2S#2.0.6` sind hardware-verifiziert. Dazu: RadioLib ^7.3 (`RADIOLIB_GODMODE=1` nötig für `spreadingFactor`-Zugriff), rweather/Crypto, densaugeo/base64. Versions-Upgrades nur auf ausdrücklichen Wunsch.

## Architektur — die Anti-Stotter-Invarianten

Kernproblem der Hardware: **E-Ink, SD-Karte und LoRa-SX1262 teilen sich denselben SPI-Bus** (HSPI, SCLK 36 / MOSI 33 / MISO 47).

```
Core 0:  Audio-Decode-Task (Prio 4) ── audio.loop() ──────┐
Core 1:  Arduino loop() ── appmgr (Eingaben/E-Ink/Apps) ──┼─ g_spiMutex (core/board.h)
Core 1:  Mesh-Pumpe (mesh_app::background → spiLock) ─────┘
```

Diese Invarianten dürfen nicht brechen:

1. **Jeder SPI-Zugriff (SD, E-Ink, LoRa) läuft unter `spiLock()`/`spiUnlock()`.**
2. **Alle `audio.*`-Aufrufe der ESP32-audioI2S-Lib nur im Audio-Task** (UI sendet Kommandos über die Queue in `services/audio.cpp`).
3. **SPI-Mutex-Freigabe während der E-Ink-BUSY-Phase** (`setBusyCallback` in `core/display.cpp`) — sonst läuft das I2S-DMA leer.
4. **256 KB PSRAM-Read-Ahead** (`audio.setBufsize(-1, 256*1024)`).
5. **SPI bleibt auf 4 MHz** (`SPI_BUS_HZ`) — 8 MHz korrumpiert SD-Writes.
6. **LoRa-CS (GPIO 3) im Leerlauf deselektiert (HIGH)**; das Radio nutzt die **bestehende `g_spi`-Instanz** (übergeben im `Module`-Konstruktor; `P_LORA_SCLK` absichtlich NICHT definiert, sonst re-initialisiert `std_init` den Bus).
7. **E-Ink-Refreshes nur bei echter Änderung** (Dirty-Flag im appmgr); Fortschritt/Statuszeile nur als `renderRegion`-Streifen.
8. **NVS (Preferences) für Settings/Bookmarks** — interner Flash, nie der SPI-Bus. SD nur für Bulk-Caches (`/.meck/id3.bin`, `/.meck/idx/`, `/books/.epub_cache/`). Mesh-Identity/Kontakte auf SPIFFS.

## Module (`src/`)

- `config.h` — alle Pins & Konstanten (aus `archive_legacy/variants/lilygo_tdeck_pro/variant.h`).
- `core/board.*` — Power, geteilter SPI-Bus, SD-Mount (+`sdReady`), `loraPower`, `g_spiMutex`.
- `core/display.*` — GxEPD2-E-Ink, `render(fn, full)`/`renderRegion(fn, y, h)`, BUSY-Callback, `cp437(true)`.
- `core/gui.*` — `toCp437`/`cp437For`/`print`/`printAt`/`textBounds`, `drawButton`, `drawRowText`, `Rect`.
- `core/touch.*` + `core/hyn/` — CST328 (I2C 13/14, nie vom SPI blockiert); hyn/ nicht refactoren.
- `core/keyboard.*` — TCA8418 (I2C 0x34, Polling, Sticky Shift/Alt/Sym, Key-Repeat 400/150 ms).
- `core/battery.*` — BQ27220-Fuel-Gauge (nur Lesen; Kalibrierung persistiert vom Legacy-Build).
- `core/settings.*` — NVS: Lautstärke, letzte App, Musik-Resume, Hörbuch-Bookmarks, Lesepositionen; `crc32`.
- `core/console.*` — Serial-Debug-Konsole (USB, `help`): `status`, `mesh init`, `advert`, `public <text>`, `dm <idx> <text>`, `contacts`, `msgs`. Sende-Befehle initialisieren das Mesh bei Bedarf automatisch — Mesh-Tests laufen damit komplett ohne Display.
- `core/appmgr.*` — App-Lifecycle (`onEnter/onLeave/tick/background/handleInput/draw`), Statuszeile (App-Name, ♫, Akku-%), Eingabe-Polling, Dirty-Koaleszenz, Anti-Doppeltap, 30-s-Persistenz, ID3-Scan-Stepping. Tap auf Statuszeile = Home.
- `services/audio.*` — pfadbasierte Abspiel-Queue (PSRAM-Staging + Commit), Owner-Token (Music/Book), Shuffle/Repeat, `seekRel` (CBR-MP3), Start-Offset, Sleep-Timer.
- `services/library.*` — SD-Scan `/music`, Künstler/Album/Playlist-Indizes, ID3-Anwendung + Cache, `indexOfPath`, inkrementeller `tagScanStep`.
- `services/id3.*` — minimaler ID3v1/v2-Reader (TIT2/TPE1/TALB, UTF-16/Latin-1→UTF-8, Frames werden ge-seekt, nicht gelesen).
- `services/textdoc.*` — Streaming-Paginierung mit Seiten-Offset-Index (Cache `/.meck/idx/`); Indexer und Renderer teilen denselben Umbruch-Code (host-getestet).
- `services/epubzip.h`/`epubproc.*` — EPUB→TXT (ROM-tinfl, OPF-Spine, XHTML-Strip → reines UTF-8), inkrementell, Cache `/books/.epub_cache/`.
- `apps/launcher.*` — Kachel-Grid, Now-Playing-/Resume-Zeile.
- `apps/music_app.*` — Browser (Künstler/Album/Playlist, Bucket-Gruppierung) + Player (Mix/Wdh/Zzz). Tasten: W/S/Enter/Backspace, A/D, X/R/Z, P, Q=Home.
- `apps/book_app.*` — Hörbücher aus `/audiobooks` (Ordner=Buch, Natural-Sort), NVS-Bookmarks (30 s + bei Pause/Leave/Eviction), ±30 s, Sleep-Timer.
- `apps/reader_app.*` — `/books` (.txt/.epub), Konvertierungs-/Index-Fortschritt (abbrechbar), Blättern per Touch-Hälften bzw. A/D.
- `apps/mesh_client.*` — `MeshClient : BaseChatMesh` (lib/meshcore), SX1262 über `g_spi`, SPIFFS-Persistenz, Nachrichten-Ringpuffer (PSRAM), Public-Channel + DMs mit ACK-Status. Alle Nachrichten (ein-/ausgehend) werden zusätzlich nach **`/meshcore/messages.log`** auf SD geschrieben (Tab-getrennt, Rotation bei 256 KB) und beim Mesh-Start die letzten ~50 zurückgeladen — ohne SD wird still übersprungen.
- `apps/mesh_app.*` — Kanal-/Kontakte-/DM-Screens mit Compose-Zeile (Tastatur tippt direkt). Radio-Init lazy beim ersten Betreten; Pumpe läuft danach in `background()` dauerhaft.
- `main.cpp` — Init-Reihenfolge: Power → Display → Touch/Tastatur/Akku/Settings → SD/Library → Audio → Apps/Launcher.

## Funkparameter

Default = **„EU/UK Narrow“** (Standard in Deutschland/NRW, identisch zu Daniels Repeater in `../meshcore_bridge_repeater/`): **869,618 MHz, BW 62,5 kHz, SF 8, CR 4/5**, 22 dBm, DIO2-RF-Switch, TCXO 2,4 V. Die platformio.ini-Flags sind nur Compile-Defaults; maßgeblich sind die NVS-Werte (`settings::meshParams()`), einstellbar in der Settings-App („Optionen“-Kachel: Preset EU Narrow/EU Klassisch oder Einzelwerte + Node-Name) und live angewandt via `mesh_client::applyRadioParams()`. Achtung Falle: `initContacts()` MUSS in `MeshClient::begin()` gerufen werden (PSRAM-Deferral des Meck-Forks), sonst crasht der erste empfangene Advert (NULL-`contacts`).

## Verifikationsstand

Am Gerät verifiziert: Display, Touch, Tastatur, Akku-Gauge, NVS, No-SD-Pfade, Audio-Task. **Mesh end-to-end bestätigt (11.06.2026, EU Narrow):** ~19 NRW-Kontakte aus Adverts, Channel-Nachrichten in beide Richtungen — eigene `#test`-Nachricht lief über 9 Hops zum Bot `D-BO-BOT-01` (Bochum), dessen `ack @[T-Deck]`-Antwort kam zurück. Kontakte überleben Reboot (SPIFFS), SD-Log + History-Reload funktionieren. Mit SD: 512-Track-Scan + ID3-Tag-Scan inkl. Cache. textdoc host-getestet.

**Drei am Gerät gefundene, gefixte Fallen (nicht reintroduzieren!):**
1. `initContacts()` muss in `MeshClient::begin()` gerufen werden — sonst NULL-`contacts`-Crash beim ersten Advert.
2. **Kein SD-Zugriff aus Mesh-Callbacks**: der Mesh-Loop hält `spiLock()`; `logToSd` direkt aus `pushMsg` war ein Self-Deadlock (nicht-rekursiver Mutex, Gerät fror beim ersten Nachrichtenempfang ein). Lösung: `s_needLog[]`-Markierung + `flushLog()` nach `spiUnlock()` in `mesh_client::loop()`.
3. **RTC-Sync**: `VolatileRTCClock` startet Mai 2024 und `bootstrapRTCfromContacts()` greift nicht (Basis gilt schon als „sane“) — Nachrichten mit altem Timestamp werden von Bots/Clients ignoriert. Lösung: Uhr in `onDiscoveredContact` aus Advert-Timestamps nach vorn syncen (`RTC=` im `status`-Befehl sichtbar).

Hinweis: `gpio_install_isr_service already installed` beim Mesh-Init ist harmlos. **Noch offen:** Hörbuch-Resume, EPUB am Gerät, DM mit ACK (braucht direkten Chat-Partner), Touch meldet bei gehaltenem Finger Dauer-Taps (kosmetisch, `[APP] Tap`-Spam im Log).
