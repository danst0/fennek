// =============================================================================
// screenshot.cpp — Host-Renderer für README-Screenshots (kein Firmware-Build).
//
// Rendert vier Bildschirme von Fennek mit Beispiel-Daten in einen 1-Bit-
// GFXcanvas1 (240x320) — exakt derselbe Adafruit-GFX-Zeichencode + dieselben
// CP437-Fonts wie auf dem E-Ink, also pixelgenau. Da die echten draw()-Methoden
// in anonymen Namespaces / hinter Geräte-Zustand liegen, sind die Zeichen-
// Bodies hier 1:1 aus den Quellen gespiegelt (Quellenangabe je Funktion);
// gemeinsame Helfer (gui::, Fonts, das Sleep-Bitmap) werden direkt verlinkt.
//
//   Bauen/Ausführen über tools/screenshots.sh
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <Adafruit_GFX.h>
#include <GxEPD2_BW.h>          // Host-Shim: GxEPD_BLACK / GxEPD_WHITE

#include "config.h"            // EINK_W / EINK_H (host-sicher)
#include "core/gui.h"
#include "core/sleep_img.h"    // kSleepImg / kSleepImgW / kSleepImgH

using gui::Rect;

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
  const char* labels[kTiles] = {"Musik", "Hörbuch", "Lesen", "Mesh", "Spiele", "Optionen"};
  const int cursor = 0;
  const char* nowPlaying = "Nuvole Bianche";

  drawStatusBar(g, "Start", 87, false, 1);

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

  drawStatusBar(g, "Spiele", 87, false, 0);

  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 12, kHeaderY, "Du bist X", 2);
  g.setTextSize(1);
  g.setCursor(12, kHeaderY + 22);
  gui::print(g, "Gegner: Fennek");

  for (int i = 1; i < 3; i++) {
    g.fillRect(kGridX + i * kCell - 1, kGridY, 3, 3 * kCell, GxEPD_BLACK);
    g.fillRect(kGridX, kGridY + i * kCell - 1, 3 * kCell, 3, GxEPD_BLACK);
  }
  for (int i = 0; i < 9; i++) drawTttMark(g, board, i);

  g.setTextSize(1);
  g.setCursor(12, kFooterY);
  gui::print(g, "N=Neu  M=Modus  Backspace=Menü");
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
  const char* nm = "Nuvole Bianche";
  const char* artist = "Ludovico Einaudi";

  drawStatusBar(g, "Musik", 87, false, 1);

  g.setTextColor(GxEPD_BLACK);
  gui::printAt(g, 6, HEADER_Y, "Wiedergabe", 2);
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

  gui::drawButton(g, kBtnList, "Liste", false);
  gui::drawButton(g, kBtnShuf, "Mix", shuffle);
  gui::drawButton(g, kBtnRep, "Wdh", repeatOn);
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
  gui::printAt(g, 120 - 53, bannerY + 6, "Fennek", 3);
  gui::printAt(g, 120 - 81, bannerY + 31, "Knopf drücken zum Aufwecken", 1);
  g.setTextColor(GxEPD_BLACK);
}

// =============================================================================
int main(int argc, char** argv) {
  std::string outdir = (argc > 1) ? argv[1] : ".";

  struct { const char* file; void (*fn)(Adafruit_GFX&); } screens[] = {
    {"launcher.pgm", screenLauncher},
    {"games-ttt.pgm", screenTtt},
    {"music.pgm", screenMusic},
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
