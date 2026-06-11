# T-Deck Pro — Minimaler MP3-Player

Schlanker MP3-Player für das **LilyGO T-Deck Pro** (ESP32-S3), von Grund auf neu
aufgebaut mit Fokus auf **responsive Touch-Bedienung** und **stotterfreie
Wiedergabe**. Der frühere MeshCore-/Meck-Code liegt vollständig unter
[`archive_legacy/`](archive_legacy/) und wird nicht mehr gebaut.

## Hardware-Realität (wichtig)

| Komponente | Detail | Konsequenz |
|---|---|---|
| Display | **E-Ink** GDEQ031T10, 240×320, monochrom | Kein LCD-Tempo. Partial-Refresh ~250–650 ms, Full-Refresh ~1 s. „Snappy" kommt aus sofortiger Eingabe-Reaktion, nicht aus schnellem Neuzeichnen. |
| Touch | CST328, **I2C** (GPIO 13/14) | Liegt **nicht** am SPI-Bus → Taps werden immer sofort erkannt, unabhängig von Refresh/Audio. |
| Audio | PCM5102A DAC über I2S (BCLK 7 / LRC 9 / DOUT 8) | Reiner I2S-DAC, keine I2C-Konfiguration. Decodierung: `ESP32-audioI2S`. |
| SD-Karte | SPI, CS 48 | **Teilt sich den SPI-Bus mit dem E-Ink.** Das ist die zentrale Stotter-Gefahr. |

## Anti-Stotter-Architektur

```
Core 0:  Audio-Decode-Task (Prio 4)  ── audio.loop() ─┐
                                                       ├─ g_spiMutex ─ geteilter SPI-Bus (E-Ink + SD)
Core 1:  Arduino loop() ── Touch + E-Ink-Render ──────┘
```

1. **Eigener Audio-Task auf Core 0.** Kein WiFi/BT aktiv → Core 0 ist exklusiv
   für die MP3-Decodierung. Die UI auf Core 1 kann ihn nicht ausbremsen.
2. **SPI-Mutex.** Jeder Buszugriff (SD-Read im Audio-Task, E-Ink-Refresh im
   UI-Task) läuft serialisiert → keine Bus-Korruption.
3. **Bus-Freigabe während der E-Ink-BUSY-Phase (der eigentliche Anti-Stotter-
   Mechanismus).** Ein Partial-Refresh dauert ~650 ms, aber davon sind nur
   ~40 ms echte SPI-Übertragung (Framebuffer schreiben); die restlichen ~600 ms
   aktualisiert der E-Ink-Controller die Anzeige **intern, ohne SPI**. Ein
   GxEPD2-`setBusyCallback` gibt den SPI-Mutex während dieser Wartephase frei →
   der Audio-Task lädt weiter von SD und füllt das I2S-DMA. Gemessen: die
   maximale `audio.loop()`-Lücke sinkt von ~600 ms auf ~40–52 ms (unter der
   DMA-Puffertiefe) → **kein Underrun.** Ohne diese Freigabe stottert es, weil
   der ~90 ms tiefe I2S-DMA-Puffer leerläuft.
4. **256 KB PSRAM-Read-Ahead.** `audio.setBufsize(-1, 256*1024)` puffert ~16 s
   codierte MP3-Daten — entkoppelt SD-Lesen vom Decodieren.
5. **Koaleszierte Refreshes.** Neu gezeichnet wird nur bei echter Änderung
   (Tap) bzw. alle 4 s für den Fortschrittsbalken — spart Strom und Bus-Zeit.

> Stotter-Test: mit `-D AUDIO_DEBUG_GAP` (in `platformio.ini`) spielt die Firmware
> beim Boot Track 0 und erzwingt alle 2 s einen Refresh; über Serial erscheint
> `[GAP] … maxGap=…ms`. Bleibt maxGap klein, ist die Wiedergabe stotterfrei.

## Bedienung (Touch)

**Hauptmenü:** `Künstler` · `Album` · `Playlist`.

**Navigation per Anfangsbuchstaben-Gruppen.** Ist eine Liste länger als ein
Bildschirm (7 Zeilen), wird sie automatisch in alphabetische Gruppen aufgeteilt
(z. B. `Abba – Cold`), bis die Auswahl ohne Scrollen auf einen Schirm passt.
Tiefe ≈ log₇(N) — bei 1000 Künstlern also 3 Tipper bis zur Einzelliste.

- **Künstler** → (Gruppen →) Künstler → Alben des Künstlers → Titel → spielt.
- **Album** → (Gruppen →) Album → Titel → spielt.
- **Playlist** → echte `.m3u`/`.m3u8`-Datei → deren Titel → spielt.
- `Zurück` geht eine Ebene hoch, `Player` springt zur laufenden Wiedergabe.
- **Player:** `|<` · `>`/`||` · `>|` · `-`/`+` (Lautstärke 0–21) · `Liste`.
- **Wiedergabe-Queue:** Weiter/Zurück und Auto-Advance folgen der gewählten
  Reihenfolge (Album- bzw. Playlist-Reihenfolge), nicht der Gesamt-Bibliothek.
- Die laufende **Zeit/Fortschrittsanzeige** aktualisiert nur ihren Streifen
  partiell (kein Ganzschirm-Refresh, kein Flackern des Rests).

### Playlists (.m3u)

`.m3u`/`.m3u8`-Dateien werden unter `/music` (rekursiv), in `/playlists` und im
Wurzelverzeichnis gefunden. Einträge dürfen absolut (`/music/…`) oder relativ zum
Playlist-Ordner sein (`\`-Pfade und `#`-Kommentare werden behandelt); jeder
Eintrag wird per Pfad-, sonst Dateinamen-Abgleich auf einen Bibliothekstitel
aufgelöst.

## Musik aufspielen

MP3-Dateien auf die SD-Karte nach **`/music`** kopieren (FAT32). Der Scan ist
**rekursiv**, d. h. `Künstler/Album/…`-Unterordner werden mitdurchsucht; leere
0-Byte-Dateien und die großen MeshCore-/Karten-Ordner (`ripple`, `tiles-bw`)
werden übersprungen. Ist `/music` leer/fehlt, wird das Wurzelverzeichnis
durchsucht. Limit: 512 Tracks (`MAX_TRACKS` in `src/config.h`).

## Build & Flash

```bash
pio run -e mp3player              # bauen
pio run -e mp3player -t upload    # flashen (USB-C)
pio device monitor -b 115200      # serielle Ausgabe
```

## Projektstruktur

```
src/
  config.h       Pins & Konstanten (aus verifiziertem Legacy-Code)
  board.*        Power-Sequenz, geteilter SPI-Bus, SD-Mount, SPI-Mutex
  display.*      E-Ink (GxEPD2), Partial/Full-Refresh
  touch.*        CST328-Wrapper
  hyn/           vendored Hynitron-Touch-Treiber (unverändert übernommen)
  library.*      SD-Scan → Track-Liste (im PSRAM)
  audio.*        ESP32-audioI2S, Decode-Task, Play/Pause/Next/Prev/Volume
  ui.*           Touch-UI: Liste + Now-Playing
  main.cpp       setup()/loop()
```

## Hardware-Status

Auf dem Gerät verifiziert (Boot-Log über USB): sauberer Boot ohne Crash, E-Ink
(Full ~1012 ms / Partial ~649 ms), Touch-Init (CST328), SD-Mount, rekursiver
Track-Scan, und **MP3-Decodierung in Echtzeit** (245 KB PSRAM-Eingangspuffer
aktiv, 44,1 kHz/16 bit/Stereo, Position läuft störungsfrei) — der Decode→I2S-Pfad
zum PCM5102A ist nachweislich aktiv.

Noch offen (nur am Gerät prüfbar, nicht aus dem Log):

- **Tonausgabe hörbar?** Decode→I2S läuft; falls kein Ton, DAC-/Verstärker-Pfad
  prüfen (`PIN_DAC_EN` GPIO 41 in `board::dacPower`).
- **Touch-Achsen:** Stimmt die Tap-Position nicht mit der Anzeige überein, die
  Flags `kSwapXY` / `kReverseX` / `kReverseY` in `src/touch.cpp` anpassen.
