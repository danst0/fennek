// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// keyboard.h — physische Tastatur (TCA8418, I2C 0x34) mit Key-Repeat.
//
// Port des hardware-verifizierten Legacy-Treibers
// (archive_legacy/variants/lilygo_tdeck_pro/Tca8418keyboard.h):
// 8x10-Matrix-Scanner, warm-reboot-sicher initialisiert, Polling über die
// Event-FIFO (kein Interrupt-Pin nötig). Sticky-Modifier Shift/Alt/Sym wie
// auf dem Tastatur-Aufdruck (Sym+W='1' usw.).
//
// Key-Repeat wird hier synthetisiert (Press-/Release-Events der FIFO):
// nach kReptDelayMs erste Wiederholung, danach alle kReptRateMs — damit
// scrollen lange Listen flott.
// =============================================================================
#pragma once

namespace keyboard {

// Initialisiert den TCA8418 (Wire muss laufen). false = nicht gefunden.
bool begin();
bool ready();

// Nächstes Zeichen ('a'..'z', Ziffern/Symbole via Sym, '\r', '\b', ' ',
// 0x02 = Mikrofon-Taste) oder 0. Liefert auch synthetische Repeats.
char poll();

}  // namespace keyboard
