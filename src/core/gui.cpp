// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "gui.h"
#include "config.h"

#include <GxEPD2_BW.h>   // GxEPD_BLACK / GxEPD_WHITE
#include <string.h>

namespace gui {

// Unicode-Codepoint -> CP437-Byte (0 = nicht darstellbar).
uint8_t cp437For(uint32_t cp) {
  if (cp < 0x80) return (uint8_t)cp;
  switch (cp) {
    // Deutsch
    case 0xE4: return 0x84;  // ä
    case 0xF6: return 0x94;  // ö
    case 0xFC: return 0x81;  // ü
    case 0xC4: return 0x8E;  // Ä
    case 0xD6: return 0x99;  // Ö
    case 0xDC: return 0x9A;  // Ü
    case 0xDF: return 0xE1;  // ß
    // Westeuropäisch (häufig in Tags/eBooks)
    case 0xE0: return 0x85;  case 0xE1: return 0xA0;  case 0xE2: return 0x83;  // à á â
    case 0xE8: return 0x8A;  case 0xE9: return 0x82;  case 0xEA: return 0x88;  // è é ê
    case 0xEB: return 0x89;  case 0xEC: return 0x8D;  case 0xED: return 0xA1;  // ë ì í
    case 0xEE: return 0x8C;  case 0xEF: return 0x8B;                            // î ï
    case 0xF2: return 0x95;  case 0xF3: return 0xA2;  case 0xF4: return 0x93;  // ò ó ô
    case 0xF9: return 0x97;  case 0xFA: return 0xA3;  case 0xFB: return 0x96;  // ù ú û
    case 0xE7: return 0x87;  case 0xC7: return 0x80;                            // ç Ç
    case 0xF1: return 0xA4;  case 0xD1: return 0xA5;                            // ñ Ñ
    case 0xE5: return 0x86;  case 0xC5: return 0x8F;                            // å Å
    case 0xE6: return 0x91;  case 0xC6: return 0x92;                            // æ Æ
    case 0xC9: return 0x90;                                                     // É
    // Satz-/Sonderzeichen
    case 0xA0: return ' ';   // NBSP
    case 0xB0: return 0xF8;  // °
    case 0xA7: return 0x15;  // §
    case 0xB5: return 0xE6;  // µ
    case 0xB1: return 0xF1;  // ±
    case 0xF7: return 0xF6;  // ÷
    case 0xAB: return 0xAE;  case 0xBB: return 0xAF;  // « »
    case 0xBF: return 0xA8;  case 0xA1: return 0xAD;  // ¿ ¡
    case 0x2022: return 0x07;                          // •
    case 0x2018: case 0x2019: case 0x201A: return '\'';
    case 0x201C: case 0x201D: case 0x201E: return '"';
    case 0x2013: case 0x2014: case 0x2212: return '-';
    default: return 0;
  }
}

void toCp437(const char* utf8, char* out, size_t outLen) {
  if (!outLen) return;
  size_t o = 0;
  const uint8_t* s = (const uint8_t*)utf8;
  while (*s && o < outLen - 1) {
    uint32_t cp;
    if (*s < 0x80) {
      cp = *s++;
    } else if ((*s & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
      cp = ((uint32_t)(*s & 0x1F) << 6) | (s[1] & 0x3F);
      s += 2;
    } else if ((*s & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
      cp = ((uint32_t)(*s & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
      s += 3;
    } else if ((*s & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
               (s[3] & 0xC0) == 0x80) {
      cp = 0;  // außerhalb BMP -> '?'
      s += 4;
    } else {
      // Ungültiges UTF-8: Byte als Latin-1 interpretieren (ID3v1 etc.).
      cp = *s++;
    }
    if (cp == 0x2026) {  // Ellipse -> "..."
      for (int k = 0; k < 3 && o < outLen - 1; k++) out[o++] = '.';
      continue;
    }
    uint8_t c = cp437For(cp);
    out[o++] = c ? (char)c : '?';
  }
  out[o] = '\0';
}

void print(Adafruit_GFX& g, const char* utf8) {
  char buf[96];
  toCp437(utf8, buf, sizeof(buf));
  g.print(buf);
}

void printAt(Adafruit_GFX& g, int x, int y, const char* utf8, uint8_t size) {
  g.setTextSize(size);
  g.setCursor(x, y);
  print(g, utf8);
}

void textBounds(Adafruit_GFX& g, const char* utf8, uint16_t* w, uint16_t* h) {
  char buf[96];
  toCp437(utf8, buf, sizeof(buf));
  int16_t bx, by;
  g.getTextBounds(buf, 0, 0, &bx, &by, w, h);
}

void drawButton(Adafruit_GFX& g, const Rect& r, const char* utf8, bool emphasize) {
  g.drawRoundRect(r.x, r.y, r.w, r.h, 6, GxEPD_BLACK);
  if (emphasize) g.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 5, GxEPD_BLACK);
  g.setTextSize(2);
  uint16_t bw, bh;
  textBounds(g, utf8, &bw, &bh);
  g.setCursor(r.x + (r.w - (int)bw) / 2, r.y + (r.h - (int)bh) / 2);
  print(g, utf8);
}

void drawRowText(Adafruit_GFX& g, int y, int rowH, const char* utf8, bool invert) {
  if (invert) g.fillRect(0, y, EINK_W, rowH, GxEPD_BLACK);
  g.setTextColor(invert ? GxEPD_WHITE : GxEPD_BLACK);
  g.setTextSize(2);
  g.setCursor(8, y + (rowH - 14) / 2);
  print(g, utf8);
  g.setTextColor(GxEPD_BLACK);
  g.drawFastHLine(0, y + rowH, EINK_W, GxEPD_BLACK);
}

}  // namespace gui
