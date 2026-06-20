// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

// =============================================================================
// podcast_core.h — Arduino-freier RSS-Parser für die Podcast-App.
//
// Bewusst header-only und ohne Arduino/SD/WiFi: damit host-testbar (siehe
// tools/host_test_apps.cpp; Präzedenz mathquiz_core/flashcards_core).
//
// Podcast-Feeds listen die Episoden in umgekehrt chronologischer Reihenfolge —
// das ERSTE <item> ist also die neueste Folge. Genau die brauchen wir
// ("immer nur die letzte Folge behalten"). Der Aufrufer (services/podcast)
// muss daher gar nicht den ganzen (oft >1 MB großen) Feed laden, sondern nur
// so viel vom Anfang, bis das erste <item> die drei Felder hergibt.
//
// Gesucht wird per Substring (kein XML-Parser an Bord), tolerant gegenüber
// Groß/Kleinschreibung, CDATA und den gängigen XML-Entities.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

namespace podcast {

constexpr int kTitleLen = 96;
constexpr int kUrlLen   = 256;
constexpr int kGuidLen  = 96;

struct EpisodeMeta {
  char          title[kTitleLen];
  char          url[kUrlLen];     // enclosure-URL (audio)
  char          guid[kGuidLen];   // stabiler Episoden-Identifier
  unsigned long lengthBytes;      // enclosure length="" (0 = unbekannt)
  bool          valid;            // url gefunden?
};

// --- kleine Helfer (intern) ---------------------------------------------------
namespace detail {

inline char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// Case-insensitive Substring-Suche in [lo, hi).
inline const char* ciFind(const char* lo, const char* hi, const char* needle) {
  size_t nl = strlen(needle);
  if (nl == 0) return lo;
  for (const char* p = lo; p + nl <= hi; ++p) {
    size_t i = 0;
    for (; i < nl; ++i)
      if (lc(p[i]) != lc(needle[i])) break;
    if (i == nl) return p;
  }
  return nullptr;
}

// Codepoint als UTF-8 anhängen.
inline void putUtf8(char* dst, size_t cap, size_t* o, uint32_t cp) {
  size_t i = *o;
  auto put = [&](char c) { if (i + 1 < cap) dst[i++] = c; };
  if (cp < 0x80) put((char)cp);
  else if (cp < 0x800) { put((char)(0xC0 | (cp >> 6))); put((char)(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { put((char)(0xE0 | (cp >> 12))); put((char)(0x80 | ((cp >> 6) & 0x3F))); put((char)(0x80 | (cp & 0x3F))); }
  else { put((char)(0xF0 | (cp >> 18))); put((char)(0x80 | ((cp >> 12) & 0x3F))); put((char)(0x80 | ((cp >> 6) & 0x3F))); put((char)(0x80 | (cp & 0x3F))); }
  *o = i;
}

// [s, e) mit XML-Entity-Dekodierung nach out kopieren, Rand-Whitespace trimmen.
inline void copyDecode(const char* s, const char* e, char* out, size_t cap) {
  out[0] = '\0';
  while (s < e && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) s++;
  while (e > s && (e[-1] == ' ' || e[-1] == '\n' || e[-1] == '\r' || e[-1] == '\t')) e--;
  size_t o = 0;
  for (const char* p = s; p < e && o + 4 < cap; ) {
    if (*p == '&') {
      const char* sc = p + 1;
      const char* semi = sc;
      while (semi < e && *semi != ';' && (size_t)(semi - sc) < 8) semi++;
      if (semi < e && *semi == ';') {
        size_t n = (size_t)(semi - sc);
        if (n >= 2 && sc[0] == '#') {              // numerische Referenz
          uint32_t cp = (sc[1] == 'x' || sc[1] == 'X')
                          ? (uint32_t)strtoul(sc + 2, nullptr, 16)
                          : (uint32_t)strtoul(sc + 1, nullptr, 10);
          if (cp) { putUtf8(out, cap, &o, cp); p = semi + 1; continue; }
        } else if (n == 3 && memcmp(sc, "amp", 3) == 0) { out[o++] = '&'; p = semi + 1; continue; }
        else if (n == 2 && memcmp(sc, "lt", 2) == 0)    { out[o++] = '<'; p = semi + 1; continue; }
        else if (n == 2 && memcmp(sc, "gt", 2) == 0)    { out[o++] = '>'; p = semi + 1; continue; }
        else if (n == 4 && memcmp(sc, "quot", 4) == 0)  { out[o++] = '"'; p = semi + 1; continue; }
        else if (n == 4 && memcmp(sc, "apos", 4) == 0)  { out[o++] = '\''; p = semi + 1; continue; }
      }
    }
    out[o++] = *p++;
  }
  out[o] = '\0';
}

// Text zwischen <tag ...> und </tag> aus [lo, hi) ziehen (inkl. CDATA-Strip).
inline bool tagText(const char* lo, const char* hi, const char* tag, char* out, size_t cap) {
  out[0] = '\0';
  char open[24]; snprintf(open, sizeof(open), "<%s", tag);
  const char* p = ciFind(lo, hi, open);
  if (!p) return false;
  p += strlen(open);
  // Tag schließen (auf '>'); Self-Closing (<guid/>) hat keinen Inhalt.
  const char* gt = (const char*)memchr(p, '>', (size_t)(hi - p));
  if (!gt) return false;
  if (gt > p && gt[-1] == '/') return false;
  p = gt + 1;
  char close[24]; snprintf(close, sizeof(close), "</%s", tag);
  const char* e = ciFind(p, hi, close);
  if (!e) e = hi;
  const char* s = p;
  while (s < e && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) s++;
  if (e - s >= 9 && memcmp(s, "<![CDATA[", 9) == 0) {
    s += 9;
    const char* cd = ciFind(s, e, "]]>");
    if (cd) e = cd;
  }
  copyDecode(s, e, out, cap);
  return out[0] != '\0';
}

// Attributwert name="..." (oder '...') innerhalb [lo, hi) lesen.
inline bool attr(const char* lo, const char* hi, const char* name, char* out, size_t cap) {
  out[0] = '\0';
  char key[24]; snprintf(key, sizeof(key), "%s=", name);
  size_t kl = strlen(key);
  for (const char* p = lo; (p = ciFind(p, hi, key)) != nullptr; p += kl) {
    // Buchstabe davor → Teiltreffer (z. B. "...url=" in einem längeren Namen).
    if (p > lo && (isalnum((unsigned char)p[-1]) || p[-1] == ':' || p[-1] == '-')) continue;
    const char* v = p + kl;
    if (v >= hi || (*v != '"' && *v != '\'')) continue;
    char q = *v++;
    const char* e = (const char*)memchr(v, q, (size_t)(hi - v));
    if (!e) e = hi;
    copyDecode(v, e, out, cap);
    return out[0] != '\0';
  }
  return false;
}

// FNV-1a-32 (für eindeutigen, deterministischen Feed-Slug-Suffix).
inline uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; ++s) { h ^= (unsigned char)*s; h *= 16777619u; }
  return h;
}

}  // namespace detail

// Erstes <item> aus dem (ggf. nur teilweise geladenen) Feed parsen.
inline EpisodeMeta parseLatest(const char* xml) {
  EpisodeMeta m; memset(&m, 0, sizeof(m)); m.valid = false;
  if (!xml) return m;
  const char* end = xml + strlen(xml);
  const char* item = detail::ciFind(xml, end, "<item");
  if (!item) return m;
  // Suchfenster bis zum nächsten <item> (sonst ganzes Pufferende) eingrenzen.
  const char* item2 = detail::ciFind(item + 5, end, "<item");
  const char* hi = item2 ? item2 : end;

  detail::tagText(item, hi, "title", m.title, sizeof(m.title));
  detail::tagText(item, hi, "guid",  m.guid,  sizeof(m.guid));

  const char* enc = detail::ciFind(item, hi, "<enclosure");
  if (enc) {
    const char* tagEnd = (const char*)memchr(enc, '>', (size_t)(hi - enc));
    if (!tagEnd) tagEnd = hi;
    detail::attr(enc, tagEnd, "url", m.url, sizeof(m.url));
    char lenbuf[24];
    if (detail::attr(enc, tagEnd, "length", lenbuf, sizeof(lenbuf)))
      m.lengthBytes = strtoul(lenbuf, nullptr, 10);
  }
  // Fallback-GUID = URL, falls der Feed kein <guid> führt.
  if (!m.guid[0] && m.url[0]) { strncpy(m.guid, m.url, sizeof(m.guid) - 1); m.guid[sizeof(m.guid) - 1] = '\0'; }
  m.valid = (m.url[0] != '\0');
  return m;
}

// Alle drei Felder beisammen? (Abbruchkriterium fürs inkrementelle Laden.)
inline bool isComplete(const EpisodeMeta& m) {
  return m.valid && m.title[0] && m.guid[0];
}

// Dateisystem-sicherer Slug aus der Feed-URL: lesbarer Host-Präfix + eindeutiger
// Hash-Suffix (zwei Feeds vom selben Host mit anderem Pfad kollidieren so nicht).
inline void feedSlug(const char* url, char* out, size_t cap) {
  out[0] = '\0';
  if (!url || cap < 12) return;
  const char* h = strstr(url, "://");
  h = h ? h + 3 : url;
  if (strncmp(h, "www.", 4) == 0) h += 4;
  size_t o = 0;
  for (const char* p = h; *p && *p != '/' && o < 16; ++p) {
    char c = detail::lc(*p);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = c;
  }
  if (o == 0) out[o++] = 'f';
  char suf[10];
  snprintf(suf, sizeof(suf), "-%08lx", (unsigned long)detail::fnv1a(url));
  for (int i = 0; suf[i] && o + 1 < cap; ++i) out[o++] = suf[i];
  out[o] = '\0';
}

// Dateiendung aus der Episoden-URL (".mp3" Default).
inline void urlExt(const char* url, char* out, size_t cap) {
  strncpy(out, ".mp3", cap - 1); out[cap - 1] = '\0';
  if (!url) return;
  const char* q = strpbrk(url, "?#");
  const char* e = q ? q : url + strlen(url);
  const char* dot = nullptr;
  for (const char* p = url; p < e; ++p) { if (*p == '.') dot = p; if (*p == '/') dot = nullptr; }
  if (!dot) return;
  size_t n = (size_t)(e - dot);
  if (n >= 2 && n <= 5) {
    static const char* ok[] = { ".mp3", ".m4a", ".aac", ".ogg", ".opus", ".wav", nullptr };
    char ext[8]; size_t i = 0;
    for (const char* p = dot; p < e && i < sizeof(ext) - 1; ++p) ext[i++] = detail::lc(*p);
    ext[i] = '\0';
    for (int k = 0; ok[k]; ++k)
      if (strcmp(ext, ok[k]) == 0) { strncpy(out, ok[k], cap - 1); out[cap - 1] = '\0'; return; }
  }
}

}  // namespace podcast
