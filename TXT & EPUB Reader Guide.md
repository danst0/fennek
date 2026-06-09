# Text & EPUB Reader Integration for Meck Firmware

## Overview

This adds a text reader accessible via the **E** key from the home screen.

**Features:**
- Browse `.txt` and `.epub` files from `/books/` folder on SD card
- Automatic EPUB-to-text conversion on first open (cached for instant re-opens)
- Word-wrapped text rendering using tiny font (maximum text density)  
- Page navigation with W/S/A/D keys
- Automatic reading position resume (persisted to SD card)
- Index files cached to SD for instant re-opens
- Bookmark indicator (`*`) on files with saved positions

**Key Mapping (T-Deck Pro):**
| Context | Key | Action |
|---------|-----|--------|
| Home screen | E | Open text reader |
| File list | W/S | Navigate up/down |
| File list | Tap / Enter | Open selected file |
| File list | Q | Back to home screen |
| Reading | W/A | Previous page |
| Reading | S/D/Space | Next page |
| Reading | Enter | Go to page number (type digits, Enter to confirm, Q to cancel) |
| Reading | Q | Close book → file list |

**Touch Gestures (T5S3):**
| Context | Gesture | Action |
|---------|---------|--------|
| File list | Swipe up/down | Scroll file list |
| File list | Tap | Open selected book |
| Reading | Tap | Next page |
| Reading | Swipe left/right | Next / previous page |
| Reading | Tap footer | Go to page number (via virtual keyboard) |
| Reading | Long press | Close book → file list |

---

## SD Card Setup

Place `.txt` or `.epub` files in a `/books/` folder on the SD card root. The reader will:
- Auto-create `/books/` if it doesn't exist
- Auto-create `/.indexes/` for page index cache files
- Auto-create `/books/.epub_cache/` for converted EPUB text
- Skip macOS hidden files (`._*`, `.DS_Store`)
- Support up to 50 files

**Index format** is compatible with the standalone reader (version 4), so if you've used the standalone reader previously, bookmarks and indexes will carry over.

---

## Cover Art & Metadata

The file list shows a **cover thumbnail** plus the book's **title** and **author**
for each `.epub`, instead of the bare filename.

**Sources (in priority order):**
1. **Sidecar files** next to the EPUB — `cover.jpg` (or `.jpeg`) and
   `metadata.opf` (`<dc:title>` / `<dc:creator>`). This is the Calibre export
   layout: `/books/<Author>/<Title>/`.
2. **Inside the EPUB** — if a sidecar is missing, the OPF is read from the ZIP
   for title/author, and the cover image is located via the manifest
   (`properties="cover-image"`, or `<meta name="cover">` → manifest item).

**Details:**
- Cover JPEGs are decoded with `JPEGDEC`, center-cropped and Bayer-dithered to a
  small 1-bit thumbnail (`READER_THUMB_W` × `READER_THUMB_H`, see
  `BookThumbnail.h`). Only JPEG covers are rendered; PNG covers fall back to a
  placeholder box.
- Title/author are converted to CP437 so umlauts render correctly in the list.
- Results are cached to `/books/.bookmeta_cache/` keyed by EPUB size, so
  re-browsing a folder is instant. Bump `BOOKMETA_VERSION` to invalidate.
- Resolution runs only for the folder you are browsing (on folder entry), not
  during the boot pre-indexer, so it never sweeps the whole tree.
- `.txt` files show a cleaned filename and a placeholder box (no cover).

| Component | Role |
|-----------|------|
| `BookThumbnail.h` | JPEG→dithered-XBM decode + OPF tag/attribute parsing |
| `EpubProcessor::getMetadata()` / `extractEntryByName()` | EPUB-internal title/author + cover bytes |
| `TextReaderScreen::resolveBookInfo()` | Sidecar-first resolution, SD cache, list rendering |

---

## EPUB Support

### How It Works

EPUB files are transparently converted to plain text on first open. The conversion pipeline is:

1. **File list** — `scanFiles()` picks up both `.txt` and `.epub` files from `/books/`
2. **First open** — `openBook()` detects the `.epub` extension and triggers conversion:
   - Shows a "Converting EPUB..." splash screen
   - Extracts the ZIP structure using ESP32-S3's built-in ROM `tinfl` decompressor (no external library needed)
   - Parses `META-INF/container.xml` → finds the OPF file
   - Parses the OPF manifest and spine to get chapters in reading order
   - Extracts each XHTML chapter, strips tags, decodes HTML entities
   - Writes concatenated plain text to `/books/.epub_cache/<filename>.txt`
3. **Subsequent opens** — the cached `.txt` is found immediately and opened like any regular text file

### Cache Structure

```
/books/
  MyBook.epub              ← original EPUB (untouched)
  SomeStory.txt            ← regular text file
  .epub_cache/
    MyBook.txt             ← auto-generated from MyBook.epub
/.indexes/
  MyBook.txt.idx           ← page index for the converted text
```

- The original `.epub` file is never modified
- Deleting a cached `.txt` from `.epub_cache/` forces re-conversion on next open
- Index files (`.idx`) work identically for both regular and EPUB-derived text files
- Boot scan picks up previously cached EPUB text files so they appear in the file list even before the EPUB is re-opened

### EPUB Processing Details

The conversion is handled by three components:

| Component | Role |
|-----------|------|
| `EpubZipReader.h` | ZIP central directory parsing + `tinfl` decompression (supports Store and Deflate) |
| `EpubProcessor.h` | EPUB structure parsing (container.xml → OPF → spine) and XHTML tag stripping |
| `TextReaderScreen.h` | Integration: detects `.epub`, triggers conversion, redirects to cached `.txt` |

**XHTML stripping handles:**
- Tag removal with block-element newlines (`<p>`, `<br>`, `<div>`, `<h1>`–`<h6>`, `<li>`, etc.)
- `<head>`, `<style>`, `<script>` content skipped entirely
- HTML entity decoding: named (`&amp;`, `&mdash;`, `&ldquo;`, etc.) and numeric (`&#8212;`, `&#x2014;`)
- Smart quote / em-dash / ellipsis → ASCII equivalents (e-ink font is ASCII-only)
- Whitespace collapsing and cleanup

**Limits:**
- Max 200 chapters in spine (`EPUB_MAX_CHAPTERS`)
- Max 256 manifest items (`EPUB_MAX_MANIFEST`)
- Manifest and chapter data are heap-allocated in PSRAM where available
- Typical conversion time: 2–10 seconds depending on book size

### Troubleshooting

| Symptom | Likely Cause |
|---------|-------------|
| "Convert failed!" splash | EPUB may be DRM-protected, corrupted, or use an unusual structure |
| EPUB appears in list but opens as blank | Check serial output for `EpubProc:` messages; chapter count may be 0 |
| Stale content after replacing an EPUB | Delete the matching `.txt` from `/books/.epub_cache/` to force re-conversion |

---

## Architecture Notes

- The reader renders through the standard `UIScreen::render()` framework, so no special bypass is needed in the main loop (unlike compose mode)
- SD card uses the same HSPI bus as e-ink display and LoRa radio — CS pin management handles contention
- Page content is pre-read from SD into a memory buffer during `handleInput()`, then rendered from buffer during `render()` — this avoids SPI bus conflicts during display refresh
- Layout metrics (chars per line, lines per page) are calculated dynamically from the display driver's font metrics on first entry
- EPUB conversion runs synchronously in `openBook()` — the e-ink splash screen keeps the user informed while the ESP32 processes the archive
- ZIP extraction uses the ESP32-S3's hardware-optimised ROM `tinfl` inflate, avoiding external compression library dependencies and the linker conflicts they cause