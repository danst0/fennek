# Changelog

All notable changes to **Fennek** (multi-app firmware for the LilyGO T-Deck Pro V1.1)
are documented here. This file covers the five most recent minor versions.

The format loosely follows [Keep a Changelog](https://keepachangelog.com/),
and the project uses [Semantic Versioning](https://semver.org/).

## [2.5.x]

### [2.5.0] — 2026-06-21

#### Fixed
- **Podcast downloads now resume and run ~7× faster.** Three issues fixed in the
  podcast sync (`services/podcast`, `apps/podcast_app`):
  - **Throughput** ~14 KB/s → ~100 KB/s: `WiFi.setSleep(false)` in `wifiUp()`.
    The Arduino-ESP32 default modem sleep parked the Wi-Fi between DTIM beacons, so
    data trickled in 16 KB bursts (~1 s apart). ~100 KB/s is now the practical
    ceiling, bounded by the 4 MHz SD write speed (invariant 5).
  - **Resume survives unclean resets**: the `.part` file is `flush()`ed every 4 MB.
    Previously FatFs committed the file size only on `f.close()`, so a power loss,
    crash, or firmware flash lost the whole partial download and the next sync
    restarted at 0. The server (Freakshow/Podlove CDN) supports HTTP Range (206) —
    the fault was device-side.
  - **Less SPI contention during app sync**: the E-Ink progress refresh is throttled
    from 1.5 s to 6 s (each partial refresh holds the shared HSPI bus ~650 ms,
    blocking SD writes).
- Added a 4 MB headless progress log line for console/pre-standby sync.

## [2.4.x]

### [2.4.7] — 2026-06-20

#### Added
- **Podcast app** (`apps/podcast_app`, launcher page 2): subscribe to RSS feeds in
  `/podcasts/feeds.txt` (editable over WebFM, seeded with the Freakshow MP3 feed),
  sync the latest episode over Wi-Fi and **keep only that one** (older audio files in
  the feed folder are deleted). The sync manages Wi-Fi itself (`audio::stop` + mesh
  suspend, like scrobble/notes_ai), fetches only the start of the (often huge) feed
  until the first `<item>` is complete, then streams the enclosure to SD in chunks
  (manual http↔https redirect resolution, network I/O outside `spiLock`). Manual sync
  (app button / `podcast sync`) covers all feeds; an opt-in **auto-sync before
  auto-standby** does one feed per standby (round-robin, RTC-RAM back-off).
- Arduino-free RSS parser `apps/podcast_core.h` (host-tested), `audio::Owner::Podcast`,
  `settings::podcastAutoSync`, console `podcast` / `podcast feed|rm|on|off|sync`.

#### Docs
- New **"Engineering around the hardware"** section on the website (`web/index.html`)
  and in all three READMEs: the four device constraints — no real-time clock, can't
  stay online, only two cores, E-Ink — and how Fennek works with them.

### [2.4.6] — 2026-06-20
- **Mental-math app:** auto-fit layout so long CFO problems no longer overflow/overlap
  (largest font whose measured width fits, block centered vertically).
- **Flashcards:** 10-card practice sessions with Fisher–Yates shuffle + box-ordering
  (least-learned first); 703-card Swedish deck under `decks/`.

### [2.4.5] — 2026-06-20
- **Mental math:** "CFO mode" replaces the earlier finance mode — large-money KPI
  mental arithmetic (share/margin, growth/YoY, VAT markup, discount, % of, Rule of 72),
  all integer results, digit-only entry.

### [2.4.4] — 2026-06-19
- **Mental math:** finance mode with percentage problems.

### [2.4.3] — 2026-06-19
- **Notes (Ollama):** strip inline `<think>` blocks so reasoning models never write
  their thought process into the polished note.

### [2.4.2] — 2026-06-19

#### Added
- **Mental Math app** (`apps/mathquiz_app`) and **Flashcards app**
  (`apps/flashcards_app`): launcher page 2. Both built on Arduino-free, host-tested
  cores (`mathquiz_core` / `flashcards_core`, `tools/host_test_apps.cpp`). Flashcards
  use Leitner boxes 0–4 with per-deck progress on SD; decks are `/flashcards/*.txt`.

### [2.4.1] — 2026-06-19

#### Added
- **Auto-polish notes via Ollama** (`services/notes_ai`): past daily notes (not
  today's) are rewritten into clean German by a local LLM. Same Wi-Fi ⊥ audio batch
  logic as scrobble — a run before auto-standby, with a SHA256-keyed done-list so only
  changed notes are re-polished. Console `ollama url/model/on/off/test/flush`.

## [2.3.x]

### [2.3.2] — 2026-06-19
- Larger homescreen page arrows and a visible back button in the Motion (gyro) app.

### [2.3.1] — 2026-06-19
- **Voice notes:** the list shows date + HH:MM, recording gain added, cleaner I2S
  hand-over back to playback after recording.

### [2.3.0] — 2026-06-19
- GPS/WAV diagnostic logs (maps-app GPS-time + mic recording level).
- Maps fallback centre moved to **Düsseldorf** (covered by the NRW tiles).
- Notes list derives the title from the WAV filename as a date/time label.

## [2.2.x]

### [2.2.4] — 2026-06-18
- Instant stop feedback when ending a recording.

### [2.2.3] — 2026-06-18
- **Recording fix:** disconnect the I2S MCLK from GPIO0 (the standby button) so
  recording no longer triggers standby.

### [2.2.2] — 2026-06-18
- Recording stops immediately; auto-standby is suppressed while recording.

### [2.2.1] — 2026-06-18

#### Added
- **Voice notes in the Notes UI:** a recording row + voice memos with playback.

### [2.2.0] — 2026-06-18

#### Added
- **Microphone recording foundation** (`services/mic`): PDM → WAV with an I2S0
  hand-over between the audio engine and the mic.
- **Gyro/Motion live data** (`apps/gyro_app`): BHI260AP accel/gyro via SensorLib.

#### Fixed
- Alarm vibration was swallowed by an old 2-bit signal clamp.

## [2.1.0] — 2026-06-17

### Added
- **Second homescreen page** (2 × 2×5 tiles) and a **Motion (gyro) app** (stage 1:
  sensor detection). Maps-app screenshots added to the docs.

## [2.0.x]

### [2.0.6] — 2026-06-17
- **Power:** disable the unused DRV2605 vibration-motor driver at boot (GPIO2 LOW,
  held across deep sleep).

### [2.0.5] — 2026-06-17
- **Power:** disable the unused GPS module at boot (GPIO39 LOW, held across sleep);
  the maps app powers it temporarily while in front.

### [2.0.4] — 2026-06-17
- Docs: Mesh-GPS verified end-to-end at the server (Antonia / meshcore.dumke.me).

### [2.0.3] — 2026-06-17
- Alarm: drop the "tone" row from the editor; console `advert flood` (multi-hop advert).

### [2.0.2] — 2026-06-17
- Alarm: the snooze button shows "+9 min" (from the engine constant).

### [2.0.1] — 2026-06-17
- **Keyboard + alarm:** holding Alt/Sym now applies to follow-up keys (so typing the
  wake time digit-by-digit works); snooze-button layout fixed.

### [2.0.0] — 2026-06-17

#### Added
- **Alarm clock** end-to-end: per-alarm signal mode (tone / blink / both) and typing
  the wake time directly on the keyboard (phone-clock logic). Loud beep at full
  volume, keyboard-backlight blink, deep-sleep wake to the alarm time. Console
  `alarm …`, app `apps/alarms_app`.

## [1.8.1] — 2026-06-13

### Changed
- **Unified back/home navigation across all apps.** Home is now a visible affordance:
  the status bar shows a house glyph (⌂) outside the launcher and the whole bar taps
  to Home. Lower corner buttons are consistently labelled "Back" (exactly one level):
  the audiobook/reader/notes lists and Settings now say "Back" instead of "Home", and
  the Settings back button steps edit → category → root → launcher. Mesh sub-screens
  (conversation/contacts/join) dropped their redundant second "Home" button (they
  already have a "Back"); the chat list and error screen relabel "Home" → "Back". The
  Files app gained a visible "Back" button, and the reader's footer tap-to-list zone
  is now labelled "◄ List". Games intentionally keep no on-screen back button — they
  exit via the status-bar Home glyph (keyboard `Backspace`/`Q` still returns to the
  games menu). Keyboard conventions (`Q` = Home, `Backspace` = one level back) are
  unchanged.

## [1.8.0] — 2026-06-13

### Added
- **Notes app** (`apps/notes_app`, launcher tile 7): a daily-notes model — exactly
  one note per day, named after the local date (`/notes/YYYY-MM-DD.md`). "+ Today"
  opens (or appends to) today's note; the list shows all days newest-first with the
  date and a first-line preview, and notes can be deleted with a confirmation. The
  editor is append-style (type / Backspace / Enter) with its own word wrap and a
  compact header. Persistence is chunked under `spiLock` (never inside `draw()`),
  with a 30 s autosave and a save on leave; empty notes are removed. Without an SD
  card it shows "No SD card" + "Search again" like the other apps. The date comes
  from the local system clock (`timesync`). New console command `notes` runs an SD
  round-trip / scan self-check (in the style of `books`).

## [1.7.1] — 2026-06-13

### Added
- **Settings backup as INI** (`services/settingsfile`): export/import all NVS
  settings to/from an SD file. SPI-free serialization on a RAM buffer, with the
  SD I/O isolated under `spiLock`. Per-book bookmarks/reading positions are
  excluded (their keys are CRC32-hashed, so paths can't be reconstructed). New
  console command + wiring in `main`/`power`.

### Docs
- Added `CHANGELOG.md` and GitHub releases for v1.3.0–v1.7.0.
- README now ships in **English (default)**, German (`README.de.md`) and
  Swedish (`README.sv.md`) with a language switcher and release/changelog badges.

## [1.7.0] — 2026-06-13

### Added
- **"+ New Channel" UI** for joining hashtag channels: a new "► New Channel"
  row at the top of the chat list opens a join screen. The keyboard types the
  name (with `#` prefix), and Enter or the "Join" button joins it (PSK derived
  from `#name`, persisted to `channels.txt` on SD). After joining, the app jumps
  straight into the new channel.

### Changed
- **Robust time synchronization without a hardware RTC.** The T-Deck Pro has no
  battery-backed RTC; the old `millis()`-based `VolatileRTCClock` lost the entire
  sleep duration across deep sleep and cold-started back to May 2024.
  - Canonical clock is now the **ESP32 system time** (`SystemRTCClock`), which
    survives deep sleep because the RTC counter keeps running while asleep.
  - New coordinator `services/timesync`: opportunistic **NTP** (when Wi-Fi is up
    via webfm) plus a short top-up just before auto-standby when quality is poor;
    Mesh adverts remain a passive source. `configTime` runs in UTC.
  - **Quality model:** `estErr = learned drift (ppm, NVS) × time since sync`,
    threshold ~5 min, with exponential back-off (RTC-RAM) to save battery when
    neither Wi-Fi nor Mesh is reachable.
  - `s_clockConfident` bootstrap: the first advert after boot / NVS restore may
    move the clock arbitrarily far forward (fixes the "+1 h cap" trap after a
    long power-off).
  - NVS fallback (`ltime`) is used only for cold start / power loss.
  - **Time zone** (POSIX-TZ, default `Europe/Berlin` incl. DST): NTP/Mesh only
    deliver UTC, so the zone is set locally; all displays use `localtime_r`.
  - Settings app: scrollable list plus new "Time zone" / "Time" (manual) rows.
  - Console: `time`, `time set`, `time sync`, `tz`.

## [1.6.x]

### [1.6.3] — 2026-06-13
- **Mesh SD persistence:** contact mirror (`/meshcore/contacts.bin`, SD takes
  load priority over SPIFFS, deferred write from `loop()`), joined hashtag
  channels (`/meshcore/channels.txt`, re-joined on boot).
- **RTC sync guard** against future-clock outliers (bootstrap + forward jumps
  < 1 h, backward only on a 3× advert consensus).
- **Contact overflow fix:** overwrite the oldest non-favorite when the contact
  table (MAX_CONTACTS=64) is full instead of silently dropping new adverts.
- Console hashtag commands `join` / `chan`.
- `i2cscan` console command: probes the I2C bus and names known devices.
  Verified on device (2026-06-13): the T-Deck Pro V1.1 has **no hardware RTC**
  (0x51 empty) → software clock + advert sync remains the way.

### [1.6.2] — 2026-06-12
- License / copyright headers (SPDX `GPL-3.0-or-later`, © Dr. Daniel Dumke)
  added to all Fennek-owned `src` files. The vendored Hynitron touch driver
  (`src/core/hyn/`) is left untouched.
- Options screen now also shows an author/copyright footer "(c) Dr. Daniel Dumke".

### [1.6.1] — 2026-06-12
- **Mesh app overhaul (MVP):** contacts split from the chat list (chats show
  only channels + contacts with history; a new contacts screen lists all
  adverts).
- HH:MM timestamps and touch scrollback in conversations.
- Status line (noise floor / RX / TX / contacts / clock).
- Contact detail view (hops + last seen).
- Touch scroll strip for lists, error-retry button, DM resend on timeout, and
  multi-ACK tracking for parallel DMs.

### [1.6.0] — 2026-06-12
- **Unlimited music library:** on-demand growing PSRAM blocks (instead of
  `MAX_TRACKS=512`), track cache `/.fennek/tracks.bin` (boot in seconds),
  background scan in `readdir` chunks. File and ID3 scans pause during playback
  (anti-stutter). Track paths up to 256 chars (Calibre).
- **Web file manager** (Files app + `webfm`): Wi-Fi station, embedded HTML page
  + JSON API to browse/upload/download/delete SD files over `http://fennek.local`.
- Console: `sleep` command + boot/reset diagnostics in `status`.

## [1.5.0] — 2026-06-12
- **Localization extended with English and Spanish** (`FENNEK_STRS` now 5
  columns, languages EN=3/ES=4 appended — NVS-compatible).
- Documented the CP437 limitation for missing uppercase accented characters.
- Reader rescan rework, chess.cpp brace fixes, and a MeshSeeLog entry.

## [1.4.0] — 2026-06-12
- **Internationalization framework** (`core/i18n`): central string table,
  language selectable in settings, applied across all apps (launcher, music,
  book, reader, mesh, games, settings).
- README screenshots + host-side renderer (`tools/screenshot.cpp`) for
  generating UI screenshots without the device.

## [1.3.x]

### [1.3.1] — 2026-06-12
- **Standby screen:** AI-generated sleeping fennec (AI Horde, AlbedoBase XL) as
  a 1-bit bitmap (Floyd–Steinberg, 240×320) instead of GFX primitives; bottom
  banner with title and wake-up hint.
- GPL-3.0+ license, README reworked as an alternative firmware.
- Repo slimmed down and detached from the Meck fork; build env renamed
  `mp3player` → `fennek`.

### [1.3.0] — 2026-06-12
- **Games app:** 2048, Minesweeper, Chess (Negamax AI), Tic-Tac-Toe.
  Game logic lives in Arduino-free, host-tested cores.

[2.4.7]: https://github.com/danst0/fennek/releases/tag/v2.4.7
[2.4.6]: https://github.com/danst0/fennek/releases/tag/v2.4.6
[2.4.5]: https://github.com/danst0/fennek/releases/tag/v2.4.5
[2.4.4]: https://github.com/danst0/fennek/releases/tag/v2.4.4
[2.4.3]: https://github.com/danst0/fennek/releases/tag/v2.4.3
[2.4.2]: https://github.com/danst0/fennek/releases/tag/v2.4.2
[2.4.1]: https://github.com/danst0/fennek/releases/tag/v2.4.1
[2.3.2]: https://github.com/danst0/fennek/releases/tag/v2.3.2
[2.3.1]: https://github.com/danst0/fennek/releases/tag/v2.3.1
[2.3.0]: https://github.com/danst0/fennek/releases/tag/v2.3.0
[2.2.4]: https://github.com/danst0/fennek/releases/tag/v2.2.4
[2.2.3]: https://github.com/danst0/fennek/releases/tag/v2.2.3
[2.2.2]: https://github.com/danst0/fennek/releases/tag/v2.2.2
[2.2.1]: https://github.com/danst0/fennek/releases/tag/v2.2.1
[2.2.0]: https://github.com/danst0/fennek/releases/tag/v2.2.0
[2.1.0]: https://github.com/danst0/fennek/releases/tag/v2.1.0
[2.0.6]: https://github.com/danst0/fennek/releases/tag/v2.0.6
[2.0.5]: https://github.com/danst0/fennek/releases/tag/v2.0.5
[2.0.4]: https://github.com/danst0/fennek/releases/tag/v2.0.4
[2.0.3]: https://github.com/danst0/fennek/releases/tag/v2.0.3
[2.0.2]: https://github.com/danst0/fennek/releases/tag/v2.0.2
[2.0.1]: https://github.com/danst0/fennek/releases/tag/v2.0.1
[2.0.0]: https://github.com/danst0/fennek/releases/tag/v2.0.0
[1.7.0]: https://github.com/danst0/fennek/releases/tag/v1.7.0
[1.6.3]: https://github.com/danst0/fennek/releases/tag/v1.6.3
[1.6.2]: https://github.com/danst0/fennek/releases/tag/v1.6.2
[1.6.1]: https://github.com/danst0/fennek/releases/tag/v1.6.1
[1.6.0]: https://github.com/danst0/fennek/releases/tag/v1.6.0
[1.5.0]: https://github.com/danst0/fennek/releases/tag/v1.5.0
[1.4.0]: https://github.com/danst0/fennek/releases/tag/v1.4.0
[1.3.1]: https://github.com/danst0/fennek/releases/tag/v1.3.1
[1.3.0]: https://github.com/danst0/fennek/releases/tag/v1.3.0
