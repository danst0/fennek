# Changelog

All notable changes to **Fennek** (multi-app firmware for the LilyGO T-Deck Pro V1.1)
are documented here. This file covers the five most recent minor versions.

The format loosely follows [Keep a Changelog](https://keepachangelog.com/),
and the project uses [Semantic Versioning](https://semver.org/).

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

[1.7.0]: https://github.com/danst0/fennek/releases/tag/v1.7.0
[1.6.3]: https://github.com/danst0/fennek/releases/tag/v1.6.3
[1.6.2]: https://github.com/danst0/fennek/releases/tag/v1.6.2
[1.6.1]: https://github.com/danst0/fennek/releases/tag/v1.6.1
[1.6.0]: https://github.com/danst0/fennek/releases/tag/v1.6.0
[1.5.0]: https://github.com/danst0/fennek/releases/tag/v1.5.0
[1.4.0]: https://github.com/danst0/fennek/releases/tag/v1.4.0
[1.3.1]: https://github.com/danst0/fennek/releases/tag/v1.3.1
[1.3.0]: https://github.com/danst0/fennek/releases/tag/v1.3.0
