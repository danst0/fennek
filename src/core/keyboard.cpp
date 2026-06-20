// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "keyboard.h"
#include "core/input.h"

#include <Arduino.h>
#include <Wire.h>

namespace {

// TCA8418-Register
constexpr uint8_t kAddr          = 0x34;
constexpr uint8_t REG_CFG        = 0x01;
constexpr uint8_t REG_INT_STAT   = 0x02;
constexpr uint8_t REG_KEY_LCK_EC = 0x03;
constexpr uint8_t REG_KEY_EVENT  = 0x04;
constexpr uint8_t REG_KP_GPIO1   = 0x1D;
constexpr uint8_t REG_KP_GPIO2   = 0x1E;
constexpr uint8_t REG_KP_GPIO3   = 0x1F;
constexpr uint8_t REG_GPI_EM1    = 0x20;
constexpr uint8_t REG_GPI_EM2    = 0x21;
constexpr uint8_t REG_GPI_EM3    = 0x22;
constexpr uint8_t REG_DEBOUNCE   = 0x29;

// Key-Repeat
constexpr uint32_t kReptDelayMs = 400;
constexpr uint32_t kReptRateMs  = 150;

bool s_ready = false;

// Sticky-Modifier (Verhalten aus dem Legacy-Treiber übernommen). Wie Shift haben
// auch Alt und Sym ein „gehalten"-Tracking: kurz getippt = sticky für EINE Taste,
// gehalten = wirkt auf ALLE Tasten bis zum Loslassen (z. B. „073" mit gehaltenem
// Alt für mehrere Ziffern, ohne Alt zwischendurch loszulassen).
bool s_shiftActive = false, s_shiftHeld = false, s_shiftUsed = false;
bool s_altActive = false, s_altHeld = false, s_altUsed = false;
bool s_symActive = false, s_symHeld = false, s_symUsed = false;

// Repeat-Zustand: zuletzt gedrückte (noch gehaltene) Taste
uint8_t  s_heldCode   = 0;
char     s_heldChar   = 0;
uint32_t s_heldSince  = 0;
uint32_t s_lastRepeat = 0;

uint8_t readReg(uint8_t reg) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom(kAddr, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

void writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// Grundbelegung (T-Deck-Pro-Matrix, Codes hardware-verifiziert).
char baseChar(uint8_t code) {
  switch (code) {
    case 10: return 'q'; case 9: return 'w'; case 8: return 'e'; case 7: return 'r';
    case 6:  return 't'; case 5: return 'y'; case 4: return 'u'; case 3: return 'i';
    case 2:  return 'o'; case 1: return 'p';
    case 20: return 'a'; case 19: return 's'; case 18: return 'd'; case 17: return 'f';
    case 16: return 'g'; case 15: return 'h'; case 14: return 'j'; case 13: return 'k';
    case 12: return 'l'; case 11: return '\b';
    case 29: return 'z'; case 28: return 'x'; case 27: return 'c'; case 26: return 'v';
    case 25: return 'b'; case 24: return 'n'; case 23: return 'm'; case 21: return '\r';
    case 33: return ' ';
    default: return 0;
  }
}

// Sym-/Alt-Ebene (Aufdruck auf den Tasten).
char symChar(uint8_t code) {
  switch (code) {
    case 10: return '#'; case 9: return '1'; case 8: return '2'; case 7: return '3';
    case 6:  return '('; case 5: return ')'; case 4: return '_'; case 3: return '-';
    case 2:  return '+'; case 1: return '@';
    case 20: return '*'; case 19: return '4'; case 18: return '5'; case 17: return '6';
    case 16: return '/'; case 15: return ':'; case 14: return ';'; case 13: return '\'';
    case 12: return '"';
    case 29: return '7'; case 28: return '8'; case 27: return '9'; case 26: return '?';
    case 25: return '!'; case 24: return ','; case 23: return '.';
    case 34: return '0';   // Sym+Mic -> 0
    default: return 0;
  }
}

// Ein FIFO-Event verarbeiten; liefert Zeichen oder 0.
char processEvent(uint8_t ev) {
  bool    pressed = (ev & 0x80) != 0;
  uint8_t code    = ev & 0x7F;

  // Release-Handling: Shift-Tracking + Repeat-Stop.
  if (!pressed) {
    if (code == 35 || code == 31) {           // Shift losgelassen
      s_shiftHeld = false;
      if (s_shiftUsed) s_shiftActive = false; // gehaltenes Shift verbraucht
      s_shiftUsed = false;
    }
    if (code == 30) {                         // Alt losgelassen
      s_altHeld = false;
      if (s_altUsed) s_altActive = false;
      s_altUsed = false;
    }
    if (code == 32) {                         // Sym losgelassen
      s_symHeld = false;
      if (s_symUsed) s_symActive = false;
      s_symUsed = false;
    }
    if (code == s_heldCode) s_heldCode = 0;   // Repeat beenden
    return 0;
  }

  // Modifier setzen (sticky + Held-Tracking).
  if (code == 35 || code == 31) { s_shiftActive = true; s_shiftHeld = true; s_shiftUsed = false; return 0; }
  if (code == 30) { s_altActive = true; s_altHeld = true; s_altUsed = false; return 0; }
  if (code == 32) { s_symActive = true; s_symHeld = true; s_symUsed = false; return 0; }

  char c = 0;
  if (code == 22) {                            // $-Taste
    c = '$';
    s_symActive = false;
  } else if (code == 34) {                     // Mikrofon-Taste
    if (s_symActive || s_altActive) {
      c = '0';
      if (s_altActive) { if (s_altHeld) s_altUsed = true; else s_altActive = false; }
      if (s_symActive) { if (s_symHeld) s_symUsed = true; else s_symActive = false; }
    }
    else c = 0x02;
  } else if (s_altActive || s_symActive) {     // Alt wirkt wie Sym (Aufdruck)
    c = symChar(code);
    if (!c) c = baseChar(code);
    // Modifier verbrauchen — gehalten bleibt aktiv (gilt für Folgetasten),
    // getippt wird nach dieser einen Taste gelöscht.
    if (s_altActive) { if (s_altHeld) s_altUsed = true; else s_altActive = false; }
    if (s_symActive) { if (s_symHeld) s_symUsed = true; else s_symActive = false; }
  } else {
    c = baseChar(code);
    if (c && s_shiftActive) {
      if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
      if (s_shiftHeld) s_shiftUsed = true;
      else             s_shiftActive = false;
    }
  }

  if (c) {
    s_heldCode   = code;
    s_heldChar   = c;
    s_heldSince  = millis();
    s_lastRepeat = 0;
  }
  return c;
}

}  // namespace

namespace keyboard {

bool begin() {
  Wire.beginTransmission(kAddr);
  if (Wire.endTransmission() != 0) return false;

  // Warm-Reboot-sichere Init-Sequenz (TCA8418 hängt nicht am Reset des ESP):
  // Scanner stoppen, alte Events leeren, Matrix konfigurieren, neu starten.
  writeReg(REG_CFG, 0x00);
  for (int i = 0; i < 16; i++) {
    if ((readReg(REG_KEY_LCK_EC) & 0x0F) == 0) break;
    readReg(REG_KEY_EVENT);
  }
  writeReg(REG_INT_STAT, 0x1F);
  writeReg(REG_GPI_EM1, 0x00);
  writeReg(REG_GPI_EM2, 0x00);
  writeReg(REG_GPI_EM3, 0x00);
  writeReg(REG_KP_GPIO1, 0xFF);   // Zeilen 0-7 als Keypad
  writeReg(REG_KP_GPIO2, 0xFF);   // Spalten 0-7
  writeReg(REG_KP_GPIO3, 0x03);   // Spalten 8-9
  writeReg(REG_DEBOUNCE, 0x03);
  writeReg(REG_INT_STAT, 0x1F);
  writeReg(REG_CFG, 0x11);        // KE_IEN + INT_CFG
  delay(5);
  for (int i = 0; i < 16; i++) {
    if ((readReg(REG_KEY_LCK_EC) & 0x0F) == 0) break;
    readReg(REG_KEY_EVENT);
  }
  writeReg(REG_INT_STAT, 0x1F);

  s_ready = true;
  return true;
}

bool ready() { return s_ready; }

char poll() {
  if (!s_ready) return 0;

  // 1) FIFO abarbeiten (kann Press- und Release-Events enthalten).
  uint8_t count = readReg(REG_KEY_LCK_EC) & 0x0F;
  while (count--) {
    uint8_t ev = readReg(REG_KEY_EVENT);
    writeReg(REG_INT_STAT, 0x1F);
    char c = processEvent(ev);
    if (c) return c;
  }

  // 2) Key-Repeat synthetisieren, solange die Taste gehalten wird.
  if (s_heldCode && s_heldChar) {
    uint32_t now = millis();
    if (now - s_heldSince >= kReptDelayMs &&
        (s_lastRepeat == 0 || now - s_lastRepeat >= kReptRateMs)) {
      s_lastRepeat = now;
      return s_heldChar;
    }
  }
  return 0;
}

}  // namespace keyboard
