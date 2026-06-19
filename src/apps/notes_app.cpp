// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "notes_app.h"
#include "config.h"
#include "core/board.h"
#include "core/display.h"
#include "core/gui.h"
#include "core/i18n.h"
#include "core/settings.h"
#include "core/power.h"
#include "services/timesync.h"
#include "services/audio.h"
#include "services/mic.h"

#include <Arduino.h>
#include <SD.h>
#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <esp_heap_caps.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

namespace {

using gui::Rect;

constexpr const char* kNotesDir = "/notes";
constexpr int kFileLen  = 28;    // "YYYY-MM-DD.md" bzw. "YYYY-MM-DD-HHMMSS.wav"
constexpr int kTitleLen = 40;    // Vorschau (erste Textzeile)
constexpr int kMaxNotes = 366;   // ein Jahr Tagesnotizen — Überlauf-Schutz
constexpr int kNoteCap  = 8192;  // RAM-Puffer pro Notiz (längere werden gekappt)

// --- Layout: Liste -----------------------------------------------------------
constexpr int W = EINK_W;
constexpr int TOP      = appmgr::CONTENT_Y;
constexpr int HEADER_Y = TOP + 2;
constexpr int HEADER_H = TOP + 26;
constexpr int ROW_H    = 30;
constexpr int LIST_Y   = HEADER_H + 4;
constexpr int VISIBLE  = 7;
constexpr int BAR_Y    = LIST_Y + VISIBLE * ROW_H + 4;

const Rect kBack    {6,   BAR_Y, 72, 44};
const Rect kUp      {84,  BAR_Y, 72, 44};
const Rect kDown    {162, BAR_Y, 72, 44};
const Rect kRetrySD {20, 150, W - 40, 48};

// --- Layout: Editor ----------------------------------------------------------
// Klassischer 6x8-Font, ganzzahlig skaliert über settings::fontScale (1/2). Der
// Textkörper nutzt die Schriftgröße; die Kopfzeile (Dateiname) bleibt klein.
// Die MAX-Werte (Größe 1) dimensionieren den statischen Zeilen-Ring; die
// effektiven Spalten/Zeilen/Zeilenhöhe stehen in s_edit* (s. applyFontScale).
constexpr int EDIT_X       = 6;
constexpr int EDIT_HDR_Y   = TOP + 3;
constexpr int EDIT_HDR_LN  = TOP + 14;   // Trennlinie unter dem Titel
constexpr int EDIT_Y       = TOP + 18;   // Textstart, unterhalb der Kopfzeile
constexpr int EDIT_COLS_MAX  = 38;
constexpr int EDIT_LINEH_MIN = 10;
constexpr int EDIT_ROWS_MAX  = (BAR_Y - 2 - EDIT_Y) / EDIT_LINEH_MIN;

int s_scale     = 1;
int s_editCols  = EDIT_COLS_MAX;
int s_editRows  = EDIT_ROWS_MAX;
int s_editLineh = EDIT_LINEH_MIN;

void applyFontScale() {
  s_scale = (int)settings::fontScale();
  if (s_scale < 1) s_scale = 1;
  s_editLineh = 10 * s_scale;                          // 8 px Glyph + Luft
  s_editCols  = (W - EDIT_X - 2) / (6 * s_scale);      // 6 px/Zeichen * Skala
  s_editRows  = (BAR_Y - 2 - EDIT_Y) / s_editLineh;
  if (s_editCols < 1) s_editCols = 1;
  if (s_editRows < 1) s_editRows = 1;
  if (s_editCols > EDIT_COLS_MAX) s_editCols = EDIT_COLS_MAX;
  if (s_editRows > EDIT_ROWS_MAX) s_editRows = EDIT_ROWS_MAX;
}

// --- Zustand -----------------------------------------------------------------
enum Screen { LIST, EDIT, CONFIRM_DEL, RECORD };
Screen s_screen = LIST;

struct NoteEntry { char file[kFileLen]; char title[kTitleLen]; bool audio; uint16_t durSec; };
NoteEntry* s_notes = nullptr;
int  s_count = -1;          // -1 = noch nicht gescannt
// Zeile 0 = "+ Heute" (Text), Zeile 1 = "Aufnahme", ab Zeile 2 die Einträge.
int  s_sel = 0, s_off = 0;
constexpr int kSpecialRows = 2;

// Aufnahme-Zustand.
char     s_recFile[kFileLen] = "";
uint32_t s_recStartMs = 0;

char*    s_buf = nullptr;    // PSRAM, aktueller Notiztext
int      s_len = 0;
char     s_curFile[kFileLen] = "";
bool     s_dirtyBuf = false;
uint32_t s_lastSave = 0;

// Editor-Schnellrefresh: gesammelte Tastendrücke (s_needEdit), zuletzt
// gezeichnetes Zeilen-Layout (für Struktur-/Scroll-Erkennung) und Zähler für
// den periodischen Sauber-Refresh gegen Ghosting.
bool s_needEdit     = false;
int  s_layLines     = 0;
int  s_layFirst     = 0;
int  s_layLastY     = 0;
int  s_fastRefreshes = 0;   // Schnell-Refreshes seit letztem Sauber-Durchlauf

void markDirty() { appmgr::markDirty(); }

bool ensureBuffers() {
  if (!s_notes) {
    s_notes = (NoteEntry*)heap_caps_malloc(sizeof(NoteEntry) * kMaxNotes, MALLOC_CAP_SPIRAM);
    if (!s_notes) s_notes = (NoteEntry*)malloc(sizeof(NoteEntry) * kMaxNotes);
  }
  if (!s_buf) {
    s_buf = (char*)heap_caps_malloc(kNoteCap, MALLOC_CAP_SPIRAM);
    if (!s_buf) s_buf = (char*)malloc(kNoteCap);
  }
  return s_notes && s_buf;
}

bool hasExt(const char* n, const char* e) {
  size_t ln = strlen(n), le = strlen(e);
  return ln > le && strcasecmp(n + ln - le, e) == 0;
}

// Neueste oben: Datumsname lexikographisch absteigend.
int cmpNote(const void* a, const void* b) {
  return strcmp(((const NoteEntry*)b)->file, ((const NoteEntry*)a)->file);
}

// Dateiname der heutigen Notiz aus der lokalen Systemzeit (Zeitzone via timesync).
void todayFile(char* out, size_t n) {
  time_t t = (time_t)timesync::now();
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, n, "%Y-%m-%d.md", &tmv);
}

// Erste Textzeile einer Notiz als Vorschau lesen (kurzer Read unter spiLock).
void readPreview(const char* path, char* out, size_t n) {
  out[0] = '\0';
  spiLock();
  File f = SD.open(path);
  if (f) {
    size_t i = 0;
    while (i + 1 < n && f.available()) {
      int c = f.read();
      if (c < 0 || c == '\n' || c == '\r') break;
      out[i++] = (char)c;
    }
    out[i] = '\0';
    f.close();
  }
  spiUnlock();
}

// /notes scannen: .md-Dateien sammeln, Vorschau lesen, nach Datum sortieren.
// Tagesnotizen sind wenige Dateien — je Datei ein kurzer Lock, kein Audio-Stau.
void scanNotes() {
  if (!ensureBuffers() || !board::sdReady()) { s_count = -1; return; }
  s_count = 0;
  spiLock();
  if (!SD.exists(kNotesDir)) SD.mkdir(kNotesDir);
  File dir = SD.open(kNotesDir);
  bool ok = dir && dir.isDirectory();
  spiUnlock();
  if (!ok) { if (dir) { spiLock(); dir.close(); spiUnlock(); } s_count = 0; return; }

  while (s_count < kMaxNotes) {
    spiLock();
    bool isDir = false;
    String entry = dir.getNextFileName(&isDir);   // readdir, kein fopen
    spiUnlock();
    if (entry.length() == 0) break;
    const char* full = entry.c_str();
    const char* nm = strrchr(full, '/');
    nm = nm ? nm + 1 : full;
    if (isDir || nm[0] == '.') continue;
    bool isMd  = hasExt(nm, ".md");
    bool isWav = hasExt(nm, ".wav");
    if (!isMd && !isWav) continue;
    NoteEntry& e = s_notes[s_count];
    strncpy(e.file, nm, kFileLen - 1); e.file[kFileLen - 1] = '\0';
    e.audio = isWav;
    e.durSec = 0;
    if (isWav) {
      // Dauer aus der Dateigröße (16 kHz mono 16-bit): (Bytes-44)/(16000*2).
      spiLock();
      File wf = SD.open(full);
      uint32_t sz = wf ? wf.size() : 0;
      if (wf) wf.close();
      spiUnlock();
      e.durSec = sz > 44 ? (uint16_t)((sz - 44) / 32000) : 0;
      // Titel aus dem Dateinamen "YYYY-MM-DD-HHMMSS.wav" -> "YYYY-MM-DD HH:MM".
      if (strlen(nm) >= 17 && nm[10] == '-')
        snprintf(e.title, kTitleLen, "%.10s %.2s:%.2s", nm, nm + 11, nm + 13);
      else
        snprintf(e.title, kTitleLen, "Sprachnotiz");
    } else {
      readPreview(full, e.title, kTitleLen);
      if (!e.title[0]) strncpy(e.title, i18n::tr(i18n::Str::EmptyList), kTitleLen - 1);
    }
    s_count++;
  }
  spiLock(); dir.close(); spiUnlock();
  qsort(s_notes, s_count, sizeof(NoteEntry), cmpNote);
  Serial.printf("[NOTES] %d Tagesnotiz(en) unter %s\n", s_count, kNotesDir);
}

// --- Datei-I/O ---------------------------------------------------------------
void notePath(const char* file, char* out, size_t n) {
  snprintf(out, n, "%s/%s", kNotesDir, file);
}

// Aktuelle Datei in s_buf laden (Cursor ans Ende → Anhänge-Modus).
void loadCurrent() {
  s_len = 0;
  s_buf[0] = '\0';
  char path[40];
  notePath(s_curFile, path, sizeof(path));
  spiLock();
  bool exists = SD.exists(path);
  spiUnlock();
  if (!exists) return;
  spiLock();
  File f = SD.open(path);
  if (f) {
    while (s_len < kNoteCap - 1) {
      int want = kNoteCap - 1 - s_len;
      if (want > 512) want = 512;
      int rd = f.read((uint8_t*)(s_buf + s_len), want);
      if (rd <= 0) break;
      s_len += rd;
    }
    s_buf[s_len] = '\0';
    f.close();
  }
  spiUnlock();
  if (s_len >= kNoteCap - 1)
    Serial.printf("[NOTES] %s gekappt auf %d Bytes\n", s_curFile, s_len);
}

// s_buf nach SD schreiben (gechunkt). Leere Notiz → Datei entfernen/gar nicht anlegen.
void saveCurrent() {
  if (!s_dirtyBuf || !board::sdReady() || !s_curFile[0]) return;
  char path[40];
  notePath(s_curFile, path, sizeof(path));
  if (s_len == 0) {
    spiLock();
    bool gone = !SD.exists(path) || SD.remove(path);
    spiUnlock();
    if (gone) s_dirtyBuf = false;   // bei Remove-Fehler dirty lassen, später erneut versuchen
    return;
  }
  spiLock();
  if (!SD.exists(kNotesDir)) SD.mkdir(kNotesDir);
  File f = SD.open(path, FILE_WRITE);   // ganze Notiz neu schreiben
  bool ok = (bool)f;
  spiUnlock();
  for (int i = 0; ok && i < s_len; ) {
    int n = (s_len - i > 512) ? 512 : s_len - i;
    spiLock();
    ok = f.write((const uint8_t*)(s_buf + i), n) == (size_t)n;
    spiUnlock();
    i += n;
  }
  spiLock();
  if (f) f.close();
  spiUnlock();
  // Dirty-Flag nur bei vollständigem Erfolg löschen — sonst geht die Notiz beim
  // nächsten openEditor (s_buf-Überschreibung) verloren, obwohl sie nie sicher
  // auf der SD landete. So bleibt sie für den nächsten Autosave-Versuch dirty.
  if (ok) {
    s_dirtyBuf = false;
    s_lastSave = millis();
  }
  Serial.printf("[NOTES] %s gespeichert (%s, %d B)\n",
                s_curFile, ok ? "ok" : "FEHLER", s_len);
}

// --- Navigation --------------------------------------------------------------
void openEditor(const char* file) {
  applyFontScale();   // aktuelle Schriftgröße -> Spalten/Zeilen/Zeilenhöhe
  strncpy(s_curFile, file, kFileLen - 1); s_curFile[kFileLen - 1] = '\0';
  loadCurrent();
  s_dirtyBuf = false;
  s_lastSave = millis();
  s_needEdit = false;
  s_fastRefreshes = 0;
  s_screen = EDIT;
  markDirty();   // erster Voll-Redraw zeichnet Titel + Body, setzt Layout-Basis
}

void openToday() {
  char today[kFileLen];
  todayFile(today, sizeof(today));
  openEditor(today);
}

void backToList() {
  saveCurrent();
  scanNotes();
  s_screen = LIST;
  markDirty();
}

void retrySD() {
  if (board::initSD()) scanNotes();
  markDirty();
}

// s_sel: 0 = "+ Heute", 1 = "Aufnahme", ab 2 die Einträge.
int rowCount() { return (s_count > 0 ? s_count : 0) + kSpecialRows; }

// --- Audionotizen ------------------------------------------------------------
void startRecord() {
  if (!board::sdReady()) return;
  time_t t = (time_t)timesync::now();
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(s_recFile, sizeof(s_recFile), "%Y-%m-%d-%H%M%S.wav", &tmv);
  char path[48];
  notePath(s_recFile, path, sizeof(path));
  audio::stop();
  if (!audio::beginMic()) { s_recFile[0] = '\0'; return; }   // I2S0-Handover
  if (!mic::startRecording(path)) { audio::endMic(); s_recFile[0] = '\0'; return; }
  s_recStartMs = millis();
  s_screen = RECORD;
  Serial.printf("[NOTES] Aufnahme gestartet: %s\n", s_recFile);
  markDirty();
}

// Schnelles Stop-Feedback (Streifen-Refresh ~0,3 s) — sofort sichtbar, BEVOR die
// langsamen Schritte (I2S0-Restore, Rescan, Voll-Refresh) laufen.
void drawStopAck(Adafruit_GFX& g) {
  g.fillRect(0, 70, W, 80, GxEPD_WHITE);
  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 30, 86, "Gespeichert", 3);
  gui::printAt(g, 30, 124, "...", 2);
}

void stopRecord() {
  if (s_screen != RECORD) return;
  Serial.println("[NOTES] Aufnahme gestoppt");
  s_screen = LIST;                                 // gegen Doppel-Stop
  mic::stopRecording();                            // WAV finalisieren (schnell)
  display::renderRegion(drawStopAck, 70, 80);      // sofortiges Feedback (~0,3 s)
  audio::endMic();                                 // I2S0 zurück an die Audio-Engine
  s_recFile[0] = '\0';
  saveCurrent();
  scanNotes();                                     // neue Aufnahme einlesen
  markDirty();                                     // Liste (Voll-Refresh)
}

void playEntry(int idx) {
  char path[48];
  notePath(s_notes[idx].file, path, sizeof(path));
  audio::queueBegin(audio::Owner::Music);
  audio::queueAdd(path);
  audio::queueCommit(0);
}

void moveSel(int delta) {
  int n = rowCount();
  int ns = s_sel + delta;
  if (ns < 0) ns = n - 1;
  if (ns >= n) ns = 0;
  s_sel = ns;
  if (s_sel < s_off) s_off = s_sel;
  if (s_sel >= s_off + VISIBLE) s_off = s_sel - VISIBLE + 1;
  markDirty();
}

void openSel() {
  if (s_sel == 0) { openToday(); return; }
  if (s_sel == 1) { startRecord(); return; }
  int idx = s_sel - kSpecialRows;
  if (idx < 0 || idx >= s_count) return;
  if (s_notes[idx].audio) playEntry(idx);
  else                    openEditor(s_notes[idx].file);
}

void deleteSel() {
  int idx = s_sel - kSpecialRows;
  if (idx < 0 || idx >= s_count) return;
  char path[48];
  notePath(s_notes[idx].file, path, sizeof(path));
  spiLock();
  if (SD.exists(path)) SD.remove(path);
  spiUnlock();
  scanNotes();
  if (s_sel >= rowCount()) s_sel = rowCount() - 1;
  s_screen = LIST;
  markDirty();
}

// --- Zeichnen: Liste ---------------------------------------------------------
void drawList(Adafruit_GFX& g) {
  gui::printAt(g, 6, HEADER_Y, i18n::tr(i18n::Str::AppNotes), 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);

  if (!board::sdReady()) {
    gui::printAt(g, 10, 80, i18n::tr(i18n::Str::NoSdCard), 2);
    gui::printAt(g, 10, 110, i18n::tr(i18n::Str::InsertCard), 1);
    gui::drawButton(g, kRetrySD, i18n::tr(i18n::Str::BtnRetrySD), false);
    return;
  }

  for (int r = 0; r < VISIBLE && s_off + r < rowCount(); r++) {
    int gi = s_off + r;
    char lbl[80];
    if (gi == 0) {
      snprintf(lbl, sizeof(lbl), "%s", i18n::tr(i18n::Str::NotesToday));
    } else if (gi == 1) {
      snprintf(lbl, sizeof(lbl), "%c Aufnahme", 0x0E);   // ♪ + Aufnahme
    } else {
      const NoteEntry& e = s_notes[gi - kSpecialRows];
      if (e.audio) {
        // Zeitanteil HH:MM:SS aus "YYYY-MM-DD-HHMMSS.wav" + Dauer.
        char tm[10] = "??:??:??";
        if (strlen(e.file) >= 17)
          snprintf(tm, sizeof(tm), "%c%c:%c%c:%c%c",
                   e.file[11], e.file[12], e.file[13], e.file[14], e.file[15], e.file[16]);
        snprintf(lbl, sizeof(lbl), "%c %s  %us", 0x0E, tm, (unsigned)e.durSec);
      } else {
        char date[kFileLen];
        strncpy(date, e.file, sizeof(date) - 1); date[sizeof(date) - 1] = '\0';
        char* dot = strrchr(date, '.');
        if (dot) *dot = '\0';   // ".md" abschneiden
        snprintf(lbl, sizeof(lbl), "%s  %s", date, e.title);
      }
    }
    gui::drawRowText(g, LIST_Y + r * ROW_H, ROW_H, lbl, false);
  }
  int cr = s_sel - s_off;
  if (cr >= 0 && cr < VISIBLE) {
    int y = LIST_Y + cr * ROW_H;
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
  }
  if (s_count == 0)
    gui::printAt(g, 10, LIST_Y + ROW_H + 12, i18n::tr(i18n::Str::NotesEmpty), 1);

  gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnBack), false);
  if (s_off > 0)                    gui::drawButton(g, kUp,   i18n::tr(i18n::Str::BtnUp), false);
  if (s_off + VISIBLE < rowCount()) gui::drawButton(g, kDown, i18n::tr(i18n::Str::BtnDown), false);
}

// --- Zeichnen: Editor --------------------------------------------------------
// Puffer in den Zeilen-Ring umbrechen und das sichtbare Fenster (letzte
// s_editRows Zeilen, Anhänge-Modus) festhalten: Zeilenzahl/erste sichtbare
// Zeile/y der untersten Zeile. Die Drei dienen dem Schnellrefresh, um
// Struktur-/Scroll-Wechsel von reinem Tippen zu trennen.
// Ring auf die MAX-Werte (Schriftgröße 1) dimensioniert; aktiv genutzt werden
// nur s_editRows/s_editCols (s. applyFontScale).
char s_ring[EDIT_ROWS_MAX][EDIT_COLS_MAX + 2];

// Wort-Umbruch: ein Wort, das nicht mehr in die Zeile passt, wandert komplett
// in die nächste (statt mitten im Wort zu trennen). Überlange Wörter (> eine
// Zeile) werden weiterhin hart umgebrochen. Notizen sind reines ASCII
// (s. onEditKey), daher gilt Spalte == Byte.
void computeLayout() {
  int  total = 0;          // Index der aktuellen Zeile im Ring
  int  col   = 0;          // bereits platzierte Spalten der aktuellen Zeile
  bool fromNewline = true; // Zeilenanfang kommt von '\n'/Pufferstart (nicht von
                           // weichem Umbruch) -> führende Leerzeichen bleiben
  char word[EDIT_COLS_MAX + 2];
  int  wlen = 0;
  s_ring[0][0] = '\0';

  auto endLine = [&](bool soft) {
    s_ring[total % s_editRows][col] = '\0';
    total++; col = 0;
    s_ring[total % s_editRows][0] = '\0';
    fromNewline = !soft;
  };
  auto placeWord = [&]() {
    if (wlen == 0) return;
    if (col > 0 && col + wlen > s_editCols) endLine(true);   // weicher Umbruch
    for (int j = 0; j < wlen; j++) s_ring[total % s_editRows][col++] = word[j];
    wlen = 0;
  };

  for (int i = 0; i < s_len; i++) {
    char c = s_buf[i];
    if (c == '\r') continue;
    if (c == '\n') { placeWord(); endLine(false); continue; }
    if (c == ' ') {
      placeWord();
      // Trenn-Leerzeichen nur setzen, wenn Platz ist und es kein weicher
      // Zeilenanfang ist (sonst beginnt eine umgebrochene Zeile mit Space).
      if (col < s_editCols && (col > 0 || fromNewline))
        s_ring[total % s_editRows][col++] = ' ';
      continue;
    }
    if (wlen >= s_editCols) placeWord();   // überlanges Wort: hart umbrechen
    word[wlen++] = c;
  }
  placeWord();

  // Cursor an die aktuelle Position hängen (auf neue Zeile, falls die volle).
  if (col >= s_editCols) endLine(true);
  s_ring[total % s_editRows][col++] = '_';
  s_ring[total % s_editRows][col]   = '\0';

  s_layLines = total + 1;
  s_layFirst = s_layLines > s_editRows ? s_layLines - s_editRows : 0;
  s_layLastY = EDIT_Y + (s_layLines - 1 - s_layFirst) * s_editLineh;
}

void drawEditor(Adafruit_GFX& g) {
  // Titel klein (Größe 1) und mit ".md" — spart Platz, Text beginnt darunter.
  gui::printAt(g, EDIT_X, EDIT_HDR_Y, s_curFile, 1);
  g.drawFastHLine(0, EDIT_HDR_LN, W, GxEPD_BLACK);

  computeLayout();
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(s_scale);
  int y = EDIT_Y;
  for (int gi = s_layFirst; gi < s_layLines; gi++) {
    g.setCursor(EDIT_X, y);
    gui::print(g, s_ring[gi % s_editRows]);
    y += s_editLineh;
  }

  gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnBackShort), false);
}

// Schnellrefresh-Streifen: nur die unterste (aktuelle) Zeile neu zeichnen.
// Setzt voraus, dass computeLayout() den Ring + s_layLastY frisch gefüllt hat.
void drawEditorLastLine(Adafruit_GFX& g) {
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(s_scale);
  g.setCursor(EDIT_X, s_layLastY);
  gui::print(g, s_ring[(s_layLines - 1) % s_editRows]);
}

// Nach (gesammelten) Tastendrücken im Editor neu zeichnen. Schnellpfad: bleibt
// das Zeilen-Layout gleich (Tippen/Löschen auf der aktuellen Zeile), reicht ein
// schmaler Streifen-Refresh der untersten Zeile — spürbar flüssiger als der
// ~650-ms-Vollausschnitt. Strukturwechsel (Umbruch/Scroll) und alle paar
// Refreshes ein Sauber-Durchlauf laufen über den vollen Redraw (markDirty), der
// auch das Ghosting-Management (alle 10 Partials ein Vollrefresh) bedient.
constexpr int kEditorClean = 8;   // nach so vielen Schnell-Refreshes einmal sauber

void flushEditorRefresh() {
  s_needEdit = false;
  if (appmgr::isDirty()) return;   // ein Vollredraw steht ohnehin an
  int oldLines = s_layLines, oldFirst = s_layFirst;
  computeLayout();
  bool structural = (s_layLines != oldLines) || (s_layFirst != oldFirst);
  if (structural || ++s_fastRefreshes >= kEditorClean) {
    s_fastRefreshes = 0;
    markDirty();   // voller Redraw, räumt periodisch das Ghosting auf
  } else {
    display::renderRegion(drawEditorLastLine, s_layLastY, s_editLineh + 4);
  }
}

void drawConfirmDel(Adafruit_GFX& g) {
  gui::printAt(g, 6, HEADER_Y, i18n::tr(i18n::Str::AppNotes), 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);
  if (s_sel >= kSpecialRows && s_sel - kSpecialRows < s_count) {
    char date[kFileLen];
    strncpy(date, s_notes[s_sel - kSpecialRows].file, sizeof(date) - 1); date[sizeof(date) - 1] = '\0';
    char* dot = strrchr(date, '.'); if (dot) *dot = '\0';
    gui::printAt(g, 10, 90, date, 2);
  }
  gui::printAt(g, 10, 130, i18n::tr(i18n::Str::NotesDelQ), 2);
  gui::drawButton(g, kBack, i18n::tr(i18n::Str::BtnBackShort), false);
}

// Aufnahme-Screen: bewusst STATISCH (kein Live-Timer/Pegel) — jeder E-Ink-Refresh
// hält den SPI-Bus und würde die Mikro-SD-Schreibvorgänge stocken lassen (Audio-
// Lücken). Dauer erscheint nach dem Stop in der Liste.
void drawRecord(Adafruit_GFX& g) {
  gui::printAt(g, 6, HEADER_Y, i18n::tr(i18n::Str::AppNotes), 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);
  gui::printAt(g, 24, 96, "Aufnahme", 4);
  gui::printAt(g, 24, 150, "Sprich jetzt ...", 2);
  gui::printAt(g, 24, 210, "Tippen / Enter = Stop", 1);
  gui::drawButton(g, kBack, "Stop", true);
}

// --- Interaktion -------------------------------------------------------------
void onListTouch(int x, int y) {
  if (kBack.hit(x, y)) { appmgr::goHome(); return; }
  if (!board::sdReady()) { if (kRetrySD.hit(x, y)) retrySD(); return; }
  if (kUp.hit(x, y)) {
    if (s_off > 0) { s_off = (s_off > VISIBLE) ? s_off - VISIBLE : 0; markDirty(); }
    return;
  }
  if (kDown.hit(x, y)) {
    if (s_off + VISIBLE < rowCount()) { s_off += VISIBLE; markDirty(); }
    return;
  }
  if (y >= LIST_Y && y < LIST_Y + VISIBLE * ROW_H) {
    int r = (y - LIST_Y) / ROW_H;
    if (s_off + r < rowCount()) { s_sel = s_off + r; openSel(); }
  }
}

void onListKey(char k) {
  switch (k) {
    case 'w': case 'W': moveSel(-1); break;
    case 's': case 'S': moveSel(+1); break;
    case '\r':          openSel(); break;
    case 'x': case 'X':
      if (s_sel >= kSpecialRows) { s_screen = CONFIRM_DEL; markDirty(); }
      break;
    case '\b':
    case 'q': case 'Q': appmgr::goHome(); break;
    default: break;
  }
}

// Editor-Tasten ändern nur den Puffer und markieren s_needEdit; den (gesammelten)
// Refresh erledigt flushEditorRefresh() in tick() — so kostet ein Tipp-Burst nur
// EINEN Bildschirm-Refresh statt einen pro Zeichen.
void onEditKey(char k) {
  if (k == '\b') {
    if (s_len == 0) { backToList(); return; }
    s_len--; s_buf[s_len] = '\0';
    s_dirtyBuf = true; s_needEdit = true;
    return;
  }
  char c = (k == '\r') ? '\n' : k;
  if (c != '\n' && (k < 32 || k > 126)) return;   // Tastatur liefert ASCII
  if (s_len < kNoteCap - 1) {
    s_buf[s_len++] = c;
    s_buf[s_len] = '\0';
    s_dirtyBuf = true; s_needEdit = true;
  }
}

// --- App-Klasse --------------------------------------------------------------
class NotesApp : public App {
 public:
  const char* id()   const override { return "Notizen"; }
  const char* name() const override { return i18n::tr(i18n::Str::AppNotes); }

  void onEnter() override {
    if (!board::sdReady()) { s_screen = LIST; return; }
    scanNotes();
    s_sel = 0; s_off = 0;
    s_screen = LIST;
  }

  void onLeave() override {
    if (s_screen == RECORD) {        // laufende Aufnahme sauber beenden (I2S0 zurück)
      mic::stopRecording();
      audio::endMic();
      s_recFile[0] = '\0';
      s_screen = LIST;
    }
    saveCurrent();   // ungesicherten Editor-Text vor App-Wechsel sichern
  }

  void handleInput(const InputEvent& e) override {
    if (e.type == InputEvent::TAP) {
      switch (s_screen) {
        case LIST: onListTouch(e.x, e.y); break;
        case EDIT: if (kBack.hit(e.x, e.y)) backToList(); break;
        case CONFIRM_DEL:
          if (kBack.hit(e.x, e.y)) { s_screen = LIST; markDirty(); }
          break;
        // Erst nach 800 ms stoppen — sonst beendet der noch gehaltene Start-Tap
        // (CST328-Dauer-Taps bei gehaltenem Finger) die Aufnahme sofort wieder.
        case RECORD: if (millis() - s_recStartMs > 800) stopRecord(); break;
      }
    } else {
      switch (s_screen) {
        case LIST: onListKey(e.key); break;
        case EDIT: onEditKey(e.key); break;
        case CONFIRM_DEL:
          if (e.key == '\r') deleteSel();
          else if (e.key == '\b' || e.key == 'q' || e.key == 'Q') { s_screen = LIST; markDirty(); }
          break;
        case RECORD: if (millis() - s_recStartMs > 800) stopRecord(); break;
      }
    }
  }

  void tick() override {
    // Gesammelte Tastendrücke nach dem Eingabe-Drain einmal zeichnen.
    if (s_screen == EDIT && s_needEdit) flushEditorRefresh();
    // Autosave im Editor: alle 30 s, wenn ungesicherte Änderungen vorliegen.
    if (s_screen == EDIT && s_dirtyBuf && millis() - s_lastSave >= 30000)
      saveCurrent();
    // Aufnahme: DMA leeren (kein Refresh hier — würde Audio stocken), wach
    // bleiben (sonst grätscht der Auto-Standby rein), Auto-Stop nach 5 min.
    if (s_screen == RECORD) {
      mic::poll();
      power::noteActivity();   // Auto-Standby nicht ins Aufnehmen grätschen lassen
      if (millis() - s_recStartMs >= 5UL * 60UL * 1000UL) stopRecord();
    }
  }

  void draw(Adafruit_GFX& g) override {
    g.setTextColor(GxEPD_BLACK);
    switch (s_screen) {
      case LIST:        drawList(g); break;
      case EDIT:        drawEditor(g); break;
      case CONFIRM_DEL: drawConfirmDel(g); break;
      case RECORD:      drawRecord(g); break;
    }
  }
};

NotesApp s_app;

}  // namespace

namespace notes_app {

App* get() { return &s_app; }

// Konsole `notes`: vollen SD-Pfad ohne UI prüfen — schreiben, zurücklesen,
// scannen, sortieren, wieder aufräumen. Nutzt eine Testdatei mit Datum in der
// Vergangenheit, damit die heutige echte Notiz unberührt bleibt.
void debugSmoke() {
  if (!ensureBuffers() || !board::sdReady()) {
    Serial.println("[NOTES] SMOKE: keine SD / kein Puffer");
    return;
  }
  const char* kTest = "1970-01-02.md";
  const char* kBody = "Smoke-Test äöü ß\nZweite Zeile.";
  strncpy(s_curFile, kTest, kFileLen - 1); s_curFile[kFileLen - 1] = '\0';
  strncpy(s_buf, kBody, kNoteCap - 1); s_buf[kNoteCap - 1] = '\0';
  s_len = (int)strlen(s_buf);
  s_dirtyBuf = true;
  saveCurrent();

  s_buf[0] = '\0'; s_len = 0;
  loadCurrent();
  bool roundtrip = (strcmp(s_buf, kBody) == 0);
  Serial.printf("[NOTES] SMOKE: Roundtrip %s (%d B zurückgelesen)\n",
                roundtrip ? "OK" : "FEHLER", s_len);

  scanNotes();
  bool found = false;
  for (int i = 0; i < s_count; i++)
    if (strcmp(s_notes[i].file, kTest) == 0) { found = true;
      Serial.printf("[NOTES] SMOKE: gelistet '%s' Vorschau='%s'\n",
                    s_notes[i].file, s_notes[i].title); }
  Serial.printf("[NOTES] SMOKE: %d Notiz(en) gescannt, Testdatei %s\n",
                s_count, found ? "gefunden" : "FEHLT");

  char path[40];
  notePath(kTest, path, sizeof(path));
  spiLock(); if (SD.exists(path)) SD.remove(path); spiUnlock();
  s_curFile[0] = '\0'; s_buf[0] = '\0'; s_len = 0; s_dirtyBuf = false;
  scanNotes();
  Serial.printf("[NOTES] SMOKE: aufgeräumt, jetzt %d Notiz(en)\n", s_count);
}

}  // namespace notes_app
