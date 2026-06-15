# Announcement post (English)

Ready-to-paste copy for r/esp32, the MeshCore community, the LilyGO forum, or
Mastodon/Bluesky. Trim the body as needed per platform.

---

## Short version (social / Mastodon / Bluesky)

> 🦊 **Fennek** — an open-source alternative firmware for the LilyGO T-Deck Pro.
> Music, audiobooks, eBooks and LoRa mesh chat (MeshCore) in one E-Ink handheld,
> with background audio that survives app switches. The fun part: E-Ink, SD and
> LoRa all share **one** SPI bus — and audio still never stutters.
>
> GPL-3.0 · ESP32-S3 · https://fennek.dumke.me · https://github.com/danst0/fennek
> #ESP32 #MeshCore #LoRa #opensource

---

## Long version (forum / Reddit)

**Title:** Fennek — an open-source multi-app firmware for the LilyGO T-Deck Pro (music, audiobooks, eBooks, LoRa mesh)

I've been building **Fennek**, a standalone alternative firmware for the
**LilyGO T-Deck Pro V1.1** (ESP32-S3, E-Ink, LoRa), and it's at a point where I'd
love to share it.

It turns the T-Deck Pro into an actual handheld with a homescreen launcher and
these apps:

- 🎵 **Music** — MP3/AAC/FLAC/WAV from SD, artist/album/playlist browser, ID3
  tags, shuffle/repeat, sleep timer, resume after reboot
- 🎧 **Audiobook** — folder-per-book, natural-sorted chapters, per-book bookmarks
- 📖 **Reading** — `.txt` and `.epub`, on-device EPUB conversion, reading position
- 📡 **Mesh** — a lean **MeshCore** client: public + hashtag channels, DMs with
  delivery status, contacts from adverts, message log on SD
- 🎮 **Games** — 2048, Minesweeper, Chess (Negamax AI), Tic-Tac-Toe
- 📝 **Notes** — one markdown note per day
- ⚙️ **Options** — LoRa radio presets/parameters, node name, battery, etc.

Touch **and** physical keyboard, plus background audio that keeps playing while
you switch apps.

**The interesting engineering bit:** on this board, E-Ink, the SD card and LoRa
all share a **single SPI bus**. The whole architecture is built around keeping
audio stutter-free anyway — a dedicated decode task on core 0 with a 256 KB PSRAM
read-ahead, every SPI access serialized behind one mutex, and the bus explicitly
released during the E-Ink BUSY phase so the I2S DMA never runs dry.

It's **GPL-3.0-or-later**, built with PlatformIO, and runs even without an SD card.
The mesh stack is based on [MeshCore](https://github.com/ripplebiz/MeshCore) by
Scott Powell.

- 🌐 Site + screenshots: **https://fennek.dumke.me**
- 💻 Source: **https://github.com/danst0/fennek**

Feedback and questions very welcome. ⚠️ Custom firmware replaces the factory
software — flash at your own risk; LilyGO's original firmware can be restored.
