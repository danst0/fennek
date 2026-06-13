// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// textdoc.h — Streaming-Paginierung für TXT-Dateien (eBook-Reader).
//
// Erster Open baut einen Seiten-Offset-Index (u32-Dateioffset pro Seite),
// häppchenweise über indexStep() (UI bleibt bedienbar), und cached ihn unter
// /.fennek/idx/<crc32>.idx (invalidiert über Dateigröße/Spalten/Zeilen).
// Seitenabruf = seek(offset) + eine Seite wort-umbrechen.
//
// Indexer und Renderer nutzen DENSELBEN Umbruch-Code — die Offsets passen
// daher exakt. Text ist UTF-8; Spaltenbreite zählt Codepoints (Anzeige
// konvertiert via gui::toCp437, 1 Codepoint = 1 Zelle).
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace textdoc {

// Dokument öffnen (cols x rows = Seitenraster). true wenn Datei lesbar.
// Bei Cache-Treffer ist indexReady() sofort true.
bool open(const char* path, int cols, int rows);

bool indexReady();
void indexStep(uint32_t budgetBytes);   // Index weiterbauen (z. B. 16 KB pro Aufruf)
int  indexPercent();                    // 0..100

int  pageCount();

// Seite p als fertig umbrochene Zeilen ('\n'-getrennt, UTF-8) liefern.
bool page(int p, char* buf, size_t bufLen);

void close();

}  // namespace textdoc
