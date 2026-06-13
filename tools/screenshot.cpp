// =============================================================================
// screenshot.cpp — Host-Renderer für README-Screenshots (kein Firmware-Build).
//
// Rendert fünf Bildschirme von Fennek mit Beispiel-Daten in einen 1-Bit-
// GFXcanvas1 (240x320) — exakt derselbe Adafruit-GFX-Zeichencode + dieselben
// CP437-Fonts wie auf dem E-Ink, also pixelgenau. Da die echten draw()-Methoden
// in anonymen Namespaces / hinter Geräte-Zustand liegen, sind die Zeichen-
// Bodies hier 1:1 aus den Quellen gespiegelt (Quellenangabe je Funktion);
// gemeinsame Helfer (gui::, Fonts, das Sleep-Bitmap) werden direkt verlinkt.
//
// Alle UI-Texte kommen aus derselben i18n-Tabelle wie die Firmware (FENNEK_STRS
// in core/i18n.h) — die Sprache wählt das 2. Argument (de/it/sv/en/es). So sind
// die Screenshots pro Sprache echt übersetzt, nicht handgepflegt.
//
//   Bauen/Ausführen über tools/screenshots.sh
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <Adafruit_GFX.h>
#include <GxEPD2_BW.h>          // Host-Shim: GxEPD_BLACK / GxEPD_WHITE

#include "config.h"            // EINK_W / EINK_H / FENNEK_VERSION (host-sicher)
#include "core/gui.h"
#include "core/i18n.h"         // FENNEK_STRS / i18n::Str (nur Header, kein .cpp)
#include "core/sleep_img.h"    // kSleepImg / kSleepImgW / kSleepImgH

using gui::Rect;
using i18n::Str;

// --- Host-Lokalisierung ------------------------------------------------------
// i18n.cpp wird bewusst NICHT gelinkt (zieht settings/NVS/Arduino). Stattdessen
// bauen wir die Tabellen direkt aus der X-Macro-Liste — exakt dieselben Texte.
namespace {
const char* const kDE[] = { "",
#define X(id, de, it, sv, en, es) de,
  FENNEK_STRS(X)
#undef X
};
const char* const kIT[] = { "",
#define X(id, de, it, sv, en, es) it,
  FENNEK_STRS(X)
#undef X
};
const char* const kSV[] = { "",
#define X(id, de, it, sv, en, es) sv,
  FENNEK_STRS(X)
#undef X
};
const char* const kEN[] = { "",
#define X(id, de, it, sv, en, es) en,
  FENNEK_STRS(X)
#undef X
};
const char* const kES[] = { "",
#define X(id, de, it, sv, en, es) es,
  FENNEK_STRS(X)
#undef X
};
const char* const* const kTab[] = { kDE, kIT, kSV, kEN, kES };           // Lang-Index
const char* const kEndonym[]    = { "Deutsch", "Italiano", "Svenska", "English", "Español" };

int g_lang = (int)i18n::Lang::EN;   // Default = Englisch (README-Default)

const char* T(Str id) {
  const char* s = kTab[g_lang][(uint16_t)id];
  return (s && s[0]) ? s : kDE[(uint16_t)id];   // leerer Eintrag -> Deutsch
}
}  // namespace

// appmgr-Konstanten (aus core/appmgr.h, ohne den Arduino-Header zu ziehen).
static constexpr int STATUS_H  = 22;
static constexpr int CONTENT_Y = STATUS_H + 2;   // 24

// --- PGM-Export (P5, 8-bit grau) ---------------------------------------------
static void savePgm(GFXcanvas1& c, const std::string& path) {
  const int w = c.width(), h = c.height();
  const uint8_t* buf = c.getBuffer();
  const int stride = (w + 7) / 8;
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) { fprintf(stderr, "kann %s nicht schreiben\n", path.c_str()); return; }
  fprintf(f, "P5\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int bit = (buf[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;  // gesetzt = weiß
      uint8_t px = bit ? 255 : 0;
      fwrite(&px, 1, 1, f);
    }
  }
  fclose(f);
}

// =============================================================================
// Statuszeile — gespiegelt aus core/appmgr.cpp (drawStatusBar/drawLock).
// =============================================================================
static void drawStatusBar(Adafruit_GFX& g, const char* name, int battPct,
                          bool battChg, uint8_t audioGlyph) {
  g.fillRect(0, 0, EINK_W, STATUS_H, GxEPD_WHITE);
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(1);
  g.setCursor(6, 7);
  gui::print(g, name ? name : "");

  if (audioGlyph) {
    g.setCursor(EINK_W - 80, 7);
    g.write((uint8_t)0x0E);
    g.print(audioGlyph == 1 ? " \x10" : " ||");
  }
  if (battPct > 0) {
    char b[8];
    snprintf(b, sizeof(b), "%d%%%s", battPct, battChg ? "+" : "");
    g.setCursor(EINK_W - 36, 7);
    g.print(b);
  }
  g.drawFastHLine(0, STATUS_H - 1, EINK_W, GxEPD_BLACK);
}

// =============================================================================
// Launcher — gespiegelt aus apps/launcher.cpp (draw + tileRect).
// =============================================================================
static void screenLauncher(Adafruit_GFX& g) {
  static constexpr int kTiles = 6;
  static constexpr int kTileW = 108, kTileH = 78;
  static const int kCol[2] = {8, 124};
  static constexpr int kRowY0 = CONTENT_Y + 6;   // 30
  static constexpr int kRowGap = 88;
  static constexpr int kHintY = 296;
  const char* labels[kTiles] = {
    T(Str::TileMusic), T(Str::TileBook), T(Str::TileReader),
    T(Str::TileMesh),  T(Str::TileGames), T(Str::TileSettings)
  };
  const int cursor = 0;
  const char* nowPlaying = "Nuvole Bianche";   // Titel — nicht übersetzen

  drawStatusBar(g, T(Str::AppLauncher), 87, false, 1);

  g.setTextColor(GxEPD_BLACK);
  for (int i = 0; i < kTiles; i++) {
    Rect r{kCol[i % 2], kRowY0 + (i / 2) * kRowGap, kTileW, kTileH};
    g.drawRoundRect(r.x, r.y, r.w, r.h, 8, GxEPD_BLACK);
    if (i == cursor)
      g.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, GxEPD_BLACK);
    g.setTextSize(2);
    uint16_t bw, bh;
    gui::textBounds(g, labels[i], &bw, &bh);
    int ty = r.y + (r.h - (int)bh) / 2;
    g.setCursor(r.x + (r.w - (int)bw) / 2, ty);
    gui::print(g, labels[i]);
  }

  g.setTextSize(1);
  g.setCursor(8, kHintY);
  char line[80];
  snprintf(line, sizeof(line), "%c %s", 0x0E, nowPlaying);   // ♫ Titel
  gui::print(g, line);
}

// =============================================================================
// Tic-Tac-Toe — gespiegelt aus apps/ttt.cpp (draw + drawMark).
// =============================================================================
static void drawTttMark(Adafruit_GFX& g, const uint8_t* board, int idx) {
  static constexpr int kCell = 72, kGridX = 12, kGridY = 64;
  int cx = kGridX + (idx % 3) * kCell + kCell / 2;
  int cy = kGridY + (idx / 3) * kCell + kCell / 2;
  if (board[idx] == 1) {
    for (int o = -1; o <= 1; o++) {
      g.drawLine(cx - 22 + o, cy - 22, cx + 22 + o, cy + 22, GxEPD_BLACK);
      g.drawLine(cx - 22 + o, cy + 22, cx + 22 + o, cy - 22, GxEPD_BLACK);
    }
  } else if (board[idx] == 2) {
    g.drawCircle(cx, cy, 24, GxEPD_BLACK);
    g.drawCircle(cx, cy, 23, GxEPD_BLACK);
    g.drawCircle(cx, cy, 22, GxEPD_BLACK);
  }
}

static void screenTtt(Adafruit_GFX& g) {
  static constexpr int kCell = 72, kGridX = 12, kGridY = 64;
  static constexpr int kHeaderY = CONTENT_Y + 4;   // 28
  static constexpr int kFooterY = 296;
  // Laufende Partie gegen Fennek: Mensch=X(1), Fennek=O(2), X am Zug.
  const uint8_t board[9] = {2, 0, 0,
                            0, 1, 0,
                            2, 0, 1};

  drawStatusBar(g, T(Str::AppGames), 87, false, 0);

  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 12, kHeaderY, T(Str::TttYouAreX), 2);
  g.setTextSize(1);
  g.setCursor(12, kHeaderY + 22);
  gui::print(g, T(Str::TttVsFennek));

  for (int i = 1; i < 3; i++) {
    g.fillRect(kGridX + i * kCell - 1, kGridY, 3, 3 * kCell, GxEPD_BLACK);
    g.fillRect(kGridX, kGridY + i * kCell - 1, 3 * kCell, 3, GxEPD_BLACK);
  }
  for (int i = 0; i < 9; i++) drawTttMark(g, board, i);

  g.setTextSize(1);
  g.setCursor(12, kFooterY);
  gui::print(g, T(Str::TttKeyHint));
}

// =============================================================================
// Musik-Player — gespiegelt aus apps/music_app.cpp (drawPlayer/drawTimeAndBar).
// =============================================================================
static void screenMusic(Adafruit_GFX& g) {
  static constexpr int W = EINK_W;
  static constexpr int HEADER_Y = CONTENT_Y + 2;   // 26
  static constexpr int HEADER_H = CONTENT_Y + 26;  // 50
  static constexpr int PROG_Y = 92;
  const Rect kBtnPrev{6, 158, 70, 52}, kBtnPlay{85, 158, 70, 52}, kBtnNext{164, 158, 70, 52};
  const Rect kBtnVolDn{6, 222, 64, 46}, kBtnVolUp{170, 222, 64, 46};
  const Rect kBtnList{6, 276, 76, 38}, kBtnShuf{88, 276, 70, 38}, kBtnRep{164, 276, 70, 38};
  // Fixture-Wiedergabezustand.
  const unsigned posSec = 87, durSec = 215;
  const int volume = 18;
  const bool playing = true, paused = false, shuffle = false;
  const bool repeatOn = false;
  const char* nm = "Nuvole Bianche";      // Titel/Künstler — nicht übersetzen
  const char* artist = "Ludovico Einaudi";

  drawStatusBar(g, T(Str::AppMusic), 87, false, 1);

  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 6, HEADER_Y, T(Str::MusicPlayback), 2);
  g.drawFastHLine(0, HEADER_H, W, GxEPD_BLACK);

  gui::printAt(g, 6, HEADER_H + 8, nm, 2);
  gui::printAt(g, 6, HEADER_H + 28, artist, 1);

  // drawTimeAndBar
  char t1[12], t2[12];
  snprintf(t1, sizeof(t1), "%u:%02u", posSec / 60, posSec % 60);
  snprintf(t2, sizeof(t2), "%u:%02u", durSec / 60, durSec % 60);
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(2);
  g.setCursor(6, PROG_Y + 8); g.print(t1);
  {
    int16_t bx, by; uint16_t bw, bh;
    g.getTextBounds(t2, 0, 0, &bx, &by, &bw, &bh);
    g.setCursor(W - 6 - (int)bw, PROG_Y + 8); g.print(t2);
  }
  int barX = 6, barY = PROG_Y + 34, barW = W - 12, barH = 12;
  g.drawRect(barX, barY, barW, barH, GxEPD_BLACK);
  {
    int fw = (int)((uint64_t)(barW - 2) * posSec / durSec);
    if (fw > 0) g.fillRect(barX + 1, barY + 1, fw, barH - 2, GxEPD_BLACK);
  }

  gui::drawButton(g, kBtnPrev, "|<", false);
  gui::drawButton(g, kBtnPlay, (playing && !paused) ? "||" : ">", true);
  gui::drawButton(g, kBtnNext, ">|", false);
  gui::drawButton(g, kBtnVolDn, "-", false);
  gui::drawButton(g, kBtnVolUp, "+", false);
  char vol[16];
  snprintf(vol, sizeof(vol), "Vol %d", volume);
  int16_t bx, by; uint16_t bw, bh;
  g.setTextSize(2);
  g.getTextBounds(vol, 0, 0, &bx, &by, &bw, &bh);
  g.setCursor((W - (int)bw) / 2, kBtnVolDn.y + (kBtnVolDn.h - (int)bh) / 2);
  g.print(vol);

  gui::drawButton(g, kBtnList, T(Str::BtnList), false);
  gui::drawButton(g, kBtnShuf, T(Str::BtnShuffle), shuffle);
  gui::drawButton(g, kBtnRep, T(Str::RepOff), repeatOn);
}

// =============================================================================
// Einstellungen — gespiegelt aus apps/settings_app.cpp (draw + rowLabel).
// =============================================================================
static void settingsSectionHeader(Adafruit_GFX& g, int y, const char* title) {
  static constexpr int SEC_H = 15;
  g.setTextSize(1);
  g.setCursor(8, y + 3);
  gui::print(g, title);
  g.drawFastHLine(0, y + SEC_H - 1, EINK_W, GxEPD_BLACK);
}

static void settingsRow(Adafruit_GFX& g, int y, const char* label,
                        const char* value, bool sel, bool arrows) {
  static constexpr int W = EINK_W, ROW_H = 24, VAL_RIGHT = EINK_W - 22;

  g.setTextSize(1);
  g.setCursor(10, y + 8);
  gui::print(g, label);

  g.setTextSize(2);
  uint16_t vw, vh;
  gui::textBounds(g, value, &vw, &vh);
  int vx = VAL_RIGHT - (int)vw;
  g.setCursor(vx, y + 5);
  gui::print(g, value);

  g.drawFastHLine(0, y + ROW_H, W, GxEPD_BLACK);

  if (sel) {
    g.drawRect(0, y, W, ROW_H, GxEPD_BLACK);
    g.drawRect(1, y + 1, W - 2, ROW_H - 2, GxEPD_BLACK);
    if (arrows) {
      g.setTextSize(1);
      g.setCursor(vx - 15, y + 8);
      g.write((uint8_t)0x11);            // ◄
      g.setCursor(W - 13, y + 8);
      g.write((uint8_t)0x10);            // ►
    }
  }
}

static void screenSettings(Adafruit_GFX& g) {
  static constexpr int TOP = CONTENT_Y;            // 24
  static constexpr int SEC_H = 15, ROW_H = 24, NUM_FUNK = 7;
  static constexpr int FUNK_Y = TOP + SEC_H;       // 39
  static constexpr int SYS_HDR_Y = FUNK_Y + NUM_FUNK * ROW_H;  // 207
  static constexpr int SYS_Y = SYS_HDR_Y + SEC_H;  // 222
  const Rect kHome{6, 274, 110, 40};
  static constexpr int FOOT_X = 120;
  // Fixture: EU-Narrow-Preset, Frequenz ausgewählt, Name "T-Deck".
  // "Preset"/"Spreading" + die Werte sind LoRa-Jargon und werden nicht übersetzt;
  // der Sprach-Wert zeigt das Endonym der gerade gerenderten Sprache.
  struct { const char* label; const char* value; } rows[] = {
    {"Preset",              "EU Narrow"},
    {T(Str::LblFreq),       "869.618 MHz"},
    {T(Str::LblBandwidth),  "62.5 kHz"},
    {"Spreading",           "SF8"},
    {T(Str::LblCodingRate), "4/5"},
    {T(Str::LblTxPower),    "22 dBm"},
    {T(Str::LblName),       "T-Deck"},
    {T(Str::SettingsLang),  kEndonym[g_lang]},
    {T(Str::LblStandby),    "5 min"},
  };
  const int sel = 1;

  drawStatusBar(g, T(Str::AppSettings), 87, false, 0);
  g.setTextColor(GxEPD_BLACK);

  settingsSectionHeader(g, TOP, T(Str::SecRadio));
  for (int r = 0; r < NUM_FUNK; r++)
    settingsRow(g, FUNK_Y + r * ROW_H, rows[r].label, rows[r].value,
                r == sel, r != 6 /* Name ohne Pfeile */);

  settingsSectionHeader(g, SYS_HDR_Y, T(Str::SecSystem));
  for (int r = NUM_FUNK; r < 9; r++)
    settingsRow(g, SYS_Y + (r - NUM_FUNK) * ROW_H, rows[r].label, rows[r].value,
                r == sel, true);

  gui::drawButton(g, kHome, T(Str::BtnHome), false);
  g.setTextSize(1);
  char bat[40];
  snprintf(bat, sizeof(bat), T(Str::FmtBattery), 87u, "", 4012u);
  g.setCursor(FOOT_X, 280);
  gui::print(g, T(Str::HintChange));
  g.setCursor(FOOT_X, 294);
  gui::print(g, bat);
  g.setCursor(FOOT_X, 308);
  char ver[32];
  snprintf(ver, sizeof(ver), "Fennek %s", FENNEK_VERSION);
  gui::print(g, ver);
}

// =============================================================================
// Sleep-Screen — gespiegelt aus core/power.cpp (drawSleepScreen).
// =============================================================================
static void screenSleep(Adafruit_GFX& g) {
  g.fillScreen(GxEPD_WHITE);
  g.drawBitmap(0, 0, kSleepImg, kSleepImgW, kSleepImgH, GxEPD_BLACK);

  const int bannerY = 278;
  g.fillRect(0, bannerY, kSleepImgW, kSleepImgH - bannerY, GxEPD_BLACK);
  g.drawFastHLine(0, bannerY, kSleepImgW, GxEPD_WHITE);
  g.setTextColor(GxEPD_WHITE);
  gui::printAt(g, 120 - 53, bannerY + 6, "Fennek", 3);   // Name — nicht übersetzen
  // Aufweck-Hinweis sprachabhängig zentrieren (Länge variiert pro Sprache).
  const char* hint = T(Str::SleepWakeHint);
  g.setTextSize(1);
  uint16_t hw, hh;
  gui::textBounds(g, hint, &hw, &hh);
  gui::printAt(g, 120 - (int)hw / 2, bannerY + 31, hint, 1);
  g.setTextColor(GxEPD_BLACK);
}

// =============================================================================
int main(int argc, char** argv) {
  std::string outdir = (argc > 1) ? argv[1] : ".";
  if (argc > 2) {
    std::string l = argv[2];
    if      (l == "de") g_lang = (int)i18n::Lang::DE;
    else if (l == "it") g_lang = (int)i18n::Lang::IT;
    else if (l == "sv") g_lang = (int)i18n::Lang::SV;
    else if (l == "en") g_lang = (int)i18n::Lang::EN;
    else if (l == "es") g_lang = (int)i18n::Lang::ES;
    else fprintf(stderr, "unbekannte Sprache '%s' — nutze Englisch\n", l.c_str());
  }

  struct { const char* file; void (*fn)(Adafruit_GFX&); } screens[] = {
    {"launcher.pgm", screenLauncher},
    {"games-ttt.pgm", screenTtt},
    {"music.pgm", screenMusic},
    {"settings.pgm", screenSettings},
    {"sleep.pgm", screenSleep},
  };

  for (auto& s : screens) {
    GFXcanvas1 canvas(EINK_W, EINK_H);
    canvas.setRotation(0);
    canvas.cp437(true);          // wie display::begin() — Umlaute/Symbole
    canvas.setTextWrap(false);
    canvas.fillScreen(GxEPD_WHITE);
    s.fn(canvas);
    savePgm(canvas, outdir + "/" + s.file);
    printf("gerendert: %s\n", s.file);
  }
  return 0;
}
