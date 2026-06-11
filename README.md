# Fennek — Multi-App-Firmware für das LilyGO T-Deck Pro

**Fennek** macht aus dem LilyGO T-Deck Pro (ESP32-S3, E-Ink, LoRa) ein echtes
Handheld: Musik hören, Hörbücher fortsetzen, eBooks lesen und über
**MeshCore** im LoRa-Mesh chatten — mit Homescreen-Launcher,
Touch + physischer Tastatur und Hintergrund-Audio über App-Grenzen hinweg.
Wie der Wüstenfuchs: klein, genügsam, große Ohren.

## Apps

| App | Funktionen |
|---|---|
| **Musik** | MP3/AAC/FLAC/WAV von SD (`/music`), Künstler/Album/Playlist-Browser, ID3-Tags (mit SD-Cache), Shuffle/Repeat, Sleep-Timer, Resume nach Reboot |
| **Hörbuch** | Bücher als Ordner unter `/audiobooks`, Kapitel natural-sortiert, Bookmark pro Buch (NVS), ±30 s-Sprünge, Sleep-Timer |
| **Lesen** | `.txt` und `.epub` aus `/books`, EPUB-Konvertierung on-device (ROM-tinfl), Seiten-Index-Cache, Leseposition pro Buch |
| **Mesh** | Schlanker MeshCore-Client: Public- und Hashtag-Kanäle (`#test` …), DMs mit Zustellstatus, Kontakte aus Adverts, Nachrichten-Log auf SD inkl. Verlauf-Reload |
| **Optionen** | Funk-Presets (EU Narrow = DE/NRW-Standard 869,618 MHz/62,5 kHz/SF8), Einzelparameter, Node-Name, Akku-Info, Firmware-Version |

Dazu: Statuszeile (Akku, Wiedergabe), Serial-Debug-Konsole über USB
(`help` eingeben — `status`, `chan test Hallo`, `meshlog`, `ls /books` …).

## Hardware

LilyGO **T-Deck Pro V1.1**: ESP32-S3 (16 MB Flash, 8 MB PSRAM), E-Ink
GDEQ031T10 240×320, CST328-Touch (I2C), TCA8418-Tastatur, PCM5102A-DAC
(I2S, 3,5-mm-Klinke), SX1262-LoRa, BQ27220-Fuel-Gauge, microSD.

**Zentrale Eigenheit:** E-Ink, SD-Karte und LoRa teilen sich **einen**
SPI-Bus. Die ganze Architektur dreht sich darum, dass Audio trotzdem nie
stottert und Eingaben sofort reagieren — Details in [`CLAUDE.md`](CLAUDE.md)
(Anti-Stotter-Invarianten).

## Bauen & Flashen

```bash
pio run -e mp3player              # bauen
pio run -e mp3player -t upload    # flashen (USB-C)
pio device monitor -b 115200      # Log + Debug-Konsole
```

Die Firmware läuft auch ohne SD-Karte (Apps zeigen dann einen Hinweis).
Funkparameter sind zur Laufzeit in der Optionen-App einstellbar;
Default ist das in Deutschland übliche **EU/UK-Narrow**-Preset.

## Projekt-Layout

- `src/` — aktive Firmware (`core/` Treiber & Framework, `services/` Audio/
  Bibliothek/Text, `apps/` die fünf Apps)
- `lib/meshcore/`, `lib/ed25519/` — vendored MeshCore-Stack (Subset)
- `archive_legacy/` — die frühere Meck-Firmware, nur noch Referenz
- `CLAUDE.md` — Architektur, Invarianten, Verifikationsstand

## Historie

Fennek entstand aus dem Meck-Fork (MeshCore-Companion für T-Deck), wurde aber
ab v1.0.0 als eigenständige Multi-App-Firmware neu aufgebaut. Der alte Code
liegt unverändert unter `archive_legacy/`.
