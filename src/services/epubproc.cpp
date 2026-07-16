// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dr. Daniel Dumke

#include "epubproc.h"
#include "epubzip.h"
#include "core/board.h"
#include "core/gui.h"
#include "core/power.h"
#include "core/settings.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <strings.h>

namespace {

constexpr int kMaxChapters = 200;
constexpr int kMaxManifest = 256;
constexpr int kXmlBufSize  = 64;

// --- Konvertierungs-Zustand (ein Job gleichzeitig) ----------------------------
struct ConvertState {
  bool   active   = false;
  bool   done     = false;
  bool   failed   = false;
  char   txtPath[192];
  File   epubFile;
  File   outFile;
  EpubZipReader* zip = nullptr;
  char** chapterPaths = nullptr;
  int    chapterCount = 0;
  int    chapterPos   = 0;
  int    written      = 0;
};
ConvertState s_cv;

// --- XML-Helfer (1:1 aus dem Legacy-Prozessor) ---------------------------------
const char* findTag(const char* data, int dataLen, const char* tag) {
  int tagLen = strlen(tag);
  const char* end = data + dataLen - tagLen;
  for (const char* p = data; p <= end; p++)
    if (memcmp(p, tag, tagLen) == 0) return p;
  return nullptr;
}

const char* findTagCI(const char* data, int dataLen, const char* tag) {
  int tagLen = strlen(tag);
  const char* end = data + dataLen - tagLen;
  for (const char* p = data; p <= end; p++)
    if (strncasecmp(p, tag, tagLen) == 0) return p;
  return nullptr;
}

bool extractAttribute(const char* data, int dataLen,
                      const char* attrName, char* outBuf, int outBufSize) {
  int nameLen = strlen(attrName);
  const char* end = data + dataLen;
  for (const char* p = data; p < end - nameLen - 2; p++) {
    if (strncmp(p, attrName, nameLen) == 0 && p[nameLen] == '=') {
      p += nameLen + 1;
      char quote = *p;
      if (quote != '"' && quote != '\'') continue;
      p++;
      const char* valEnd = (const char*)memchr(p, quote, end - p);
      if (!valEnd) continue;
      int valLen = valEnd - p;
      if (valLen >= outBufSize) valLen = outBufSize - 1;
      memcpy(outBuf, p, valLen);
      outBuf[valLen] = '\0';
      return true;
    }
  }
  return false;
}

bool extractTagContent(const char* data, int dataLen,
                       const char* tagName, char* outBuf, int outBufSize) {
  char openTag[64];
  snprintf(openTag, sizeof(openTag), "<%s", tagName);
  const char* start = findTag(data, dataLen, openTag);
  if (!start) return false;
  const char* end = data + dataLen;
  const char* contentStart = (const char*)memchr(start, '>', end - start);
  if (!contentStart) return false;
  contentStart++;
  char closeTag[64];
  snprintf(closeTag, sizeof(closeTag), "</%s>", tagName);
  const char* contentEnd = findTag(contentStart, end - contentStart, closeTag);
  if (!contentEnd) return false;
  int len = contentEnd - contentStart;
  if (len >= outBufSize) len = outBufSize - 1;
  memcpy(outBuf, contentStart, len);
  outBuf[len] = '\0';
  return true;
}

void getDirectory(const char* path, char* dirBuf, int dirBufSize) {
  const char* lastSlash = strrchr(path, '/');
  if (lastSlash) {
    int len = lastSlash - path + 1;
    if (len >= dirBufSize) len = dirBufSize - 1;
    memcpy(dirBuf, path, len);
    dirBuf[len] = '\0';
  } else {
    dirBuf[0] = '\0';
  }
}

bool findOpfPath(EpubZipReader* zip, char* opfPath, int opfPathSize) {
  int idx = zip->findEntry("META-INF/container.xml");
  if (idx < 0) {
    idx = zip->findEntryBySuffix(".opf");
    if (idx >= 0) {
      const ZipEntry* e = zip->getEntry(idx);
      strncpy(opfPath, e->filename, opfPathSize - 1);
      opfPath[opfPathSize - 1] = '\0';
      return true;
    }
    return false;
  }
  uint32_t size = 0;
  uint8_t* data = zip->extractEntry(idx, &size);
  if (!data) return false;
  bool found = extractAttribute((const char*)data, size, "full-path", opfPath, opfPathSize);
  free(data);
  return found;
}

void freeChapterPaths(char** paths, int count) {
  if (!paths) return;
  for (int i = 0; i < count; i++)
    if (paths[i]) free(paths[i]);
  free(paths);
}

bool parseOpf(EpubZipReader* zip, const char* opfPath, const char* baseDir,
              char* title, int titleSize, char*** outChapterPaths, int* outChapterCount) {
  int opfIdx = zip->findEntry(opfPath);
  if (opfIdx < 0) return false;
  uint32_t opfSize = 0;
  uint8_t* opfData = zip->extractEntry(opfIdx, &opfSize);
  if (!opfData) return false;
  const char* xml = (const char*)opfData;

  extractTagContent(xml, opfSize, "dc:title", title, titleSize);

  struct ManifestItem {
    char id[64];
    char href[128];
    bool isContent;
  };
  ManifestItem* manifest = (ManifestItem*)ps_malloc(kMaxManifest * sizeof(ManifestItem));
  if (!manifest) manifest = (ManifestItem*)malloc(kMaxManifest * sizeof(ManifestItem));
  if (!manifest) { free(opfData); return false; }
  int manifestCount = 0;

  const char* manifestStart = findTag(xml, opfSize, "<manifest");
  const char* manifestEnd = manifestStart
      ? findTag(manifestStart, opfSize - (manifestStart - xml), "</manifest") : nullptr;
  if (!manifestEnd) manifestEnd = xml + opfSize;

  if (manifestStart) {
    const char* pos = manifestStart;
    while (pos < manifestEnd && manifestCount < kMaxManifest) {
      pos = findTag(pos, manifestEnd - pos, "<item");
      if (!pos || pos >= manifestEnd) break;
      const char* tagEnd = (const char*)memchr(pos, '>', manifestEnd - pos);
      if (!tagEnd) break;
      tagEnd++;

      ManifestItem& item = manifest[manifestCount];
      item.id[0] = '\0'; item.href[0] = '\0'; item.isContent = false;
      extractAttribute(pos, tagEnd - pos, "id", item.id, sizeof(item.id));
      extractAttribute(pos, tagEnd - pos, "href", item.href, sizeof(item.href));
      char mediaType[64] = "";
      extractAttribute(pos, tagEnd - pos, "media-type", mediaType, sizeof(mediaType));
      item.isContent = (strstr(mediaType, "html") != nullptr ||
                        strstr(mediaType, "xml") != nullptr);
      if (item.id[0] && item.href[0]) manifestCount++;
      pos = tagEnd;
    }
  }

  const char* spineStart = findTag(xml, opfSize, "<spine");
  const char* spineEnd = spineStart
      ? findTag(spineStart, opfSize - (spineStart - xml), "</spine") : nullptr;
  if (!spineEnd) spineEnd = xml + opfSize;

  char** chapterPaths = (char**)ps_malloc(kMaxChapters * sizeof(char*));
  if (!chapterPaths) chapterPaths = (char**)malloc(kMaxChapters * sizeof(char*));
  if (!chapterPaths) { free(manifest); free(opfData); return false; }
  int chapterCount = 0;

  if (spineStart) {
    const char* pos = spineStart;
    while (pos < spineEnd && chapterCount < kMaxChapters) {
      pos = findTag(pos, spineEnd - pos, "<itemref");
      if (!pos || pos >= spineEnd) break;
      const char* tagEnd = (const char*)memchr(pos, '>', spineEnd - pos);
      if (!tagEnd) break;
      tagEnd++;

      char idref[64] = "";
      extractAttribute(pos, tagEnd - pos, "idref", idref, sizeof(idref));
      if (idref[0]) {
        for (int m = 0; m < manifestCount; m++) {
          if (strcmp(manifest[m].id, idref) == 0 && manifest[m].isContent) {
            int pathLen = strlen(baseDir) + strlen(manifest[m].href) + 1;
            char* fullPath = (char*)malloc(pathLen);
            if (fullPath) {
              snprintf(fullPath, pathLen, "%s%s", baseDir, manifest[m].href);
              chapterPaths[chapterCount++] = fullPath;
            }
            break;
          }
        }
      }
      pos = tagEnd;
    }
  }

  free(manifest);
  free(opfData);
  *outChapterPaths = chapterPaths;
  *outChapterCount = chapterCount;
  return chapterCount > 0;
}

// HTML-Entity ab '&' dekodieren; liefert Unicode-Codepoint (0 = überspringen).
// *outPos zeigt danach auf das letzte konsumierte Zeichen.
uint32_t decodeEntity(const uint8_t* p, const uint8_t* end, const uint8_t** outPos) {
  const uint8_t* semi = p + 1;
  int maxLen = 10;
  while (semi < end && semi < p + maxLen && *semi != ';') semi++;
  if (semi >= end || *semi != ';') { *outPos = p; return '&'; }

  int entityLen = semi - p - 1;
  const char* e = (const char*)(p + 1);
  *outPos = semi;

  struct Ent { const char* name; uint32_t cp; };
  static const Ent kEnts[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", ' '}, {"mdash", '-'}, {"ndash", '-'}, {"hellip", '.'},
    {"lsquo", '\''}, {"rsquo", '\''}, {"ldquo", '"'}, {"rdquo", '"'},
    {"auml", 0xE4}, {"ouml", 0xF6}, {"uuml", 0xFC}, {"szlig", 0xDF},
    {"Auml", 0xC4}, {"Ouml", 0xD6}, {"Uuml", 0xDC},
    {"eacute", 0xE9}, {"egrave", 0xE8}, {"ecirc", 0xEA}, {"euml", 0xEB},
    {"agrave", 0xE0}, {"aacute", 0xE1}, {"acirc", 0xE2},
    {"ccedil", 0xE7}, {"Ccedil", 0xC7}, {"Eacute", 0xC9},
    {"iacute", 0xED}, {"icirc", 0xEE}, {"iuml", 0xEF}, {"igrave", 0xEC},
    {"oacute", 0xF3}, {"ocirc", 0xF4}, {"ograve", 0xF2},
    {"uacute", 0xFA}, {"ucirc", 0xFB}, {"ugrave", 0xF9},
    {"ntilde", 0xF1}, {"Ntilde", 0xD1},
  };
  for (const Ent& en : kEnts) {
    if ((int)strlen(en.name) == entityLen && strncmp(e, en.name, entityLen) == 0)
      return en.cp;
  }

  // Numerisch: &#NNN; bzw. &#xHH;
  if (entityLen >= 2 && e[0] == '#') {
    uint32_t cp = 0;
    if (e[1] == 'x' || e[1] == 'X') {
      for (int i = 2; i < entityLen; i++) {
        char ch = e[i];
        if (ch >= '0' && ch <= '9') cp = cp * 16 + (ch - '0');
        else if (ch >= 'a' && ch <= 'f') cp = cp * 16 + (ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') cp = cp * 16 + (ch - 'A' + 10);
      }
    } else {
      for (int i = 1; i < entityLen; i++) {
        char ch = e[i];
        if (ch >= '0' && ch <= '9') cp = cp * 10 + (ch - '0');
      }
    }
    return cp;
  }
  return ' ';
}

// Typografische Codepoints auf ASCII abbilden (0 = keine Sonderbehandlung).
char mapTypographic(uint32_t cp) {
  switch (cp) {
    case 0x2018: case 0x2019: case 0x2032: case 0x2039: case 0x203A: return '\'';
    case 0x201C: case 0x201D: case 0x2033: case 0x00AB: case 0x00BB: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013:
    case 0x2014: case 0x2015: return '-';
    case 0x2026: return '.';
    case 0x2022: return '*';
    case 0x00A0: return ' ';
    default: return 0;
  }
}

// XHTML -> reines UTF-8 (Tags raus, Blockelemente -> Zeilenumbruch,
// Entities dekodieren, Whitespace kollabieren). Aus dem Legacy-Code, aber
// mit durchgängig UTF-8-kodiertem Output.
uint8_t* stripXhtml(const uint8_t* input, uint32_t inputLen, uint32_t* outLen) {
  // Output kann durch Entity-Expansion minimal wachsen — 1/8 Reserve.
  uint8_t* output = (uint8_t*)ps_malloc(inputLen + inputLen / 8 + 16);
  if (!output) output = (uint8_t*)malloc(inputLen + inputLen / 8 + 16);
  if (!output) { *outLen = 0; return nullptr; }

  uint32_t outPos = 0;
  bool inTag = false;
  bool skipContent = false;
  char tagName[32];
  int  tagNamePos = 0;
  bool tagNameDone = false;
  bool isClosingTag = false;
  bool lastWasNewline = false;
  bool lastWasSpace = false;

  auto emitCp = [&](uint32_t cp) {
    char t = mapTypographic(cp);
    if (t) cp = (uint32_t)t;
    if (cp == 0) return;
    if (cp >= 0x80 && !gui::cp437For(cp)) return;   // im Font nicht darstellbar
    if (cp < 0x80) {
      output[outPos++] = (uint8_t)cp;
    } else if (cp <= 0x7FF) {
      output[outPos++] = 0xC0 | (cp >> 6);
      output[outPos++] = 0x80 | (cp & 0x3F);
    } else if (cp <= 0xFFFF) {
      output[outPos++] = 0xE0 | (cp >> 12);
      output[outPos++] = 0x80 | ((cp >> 6) & 0x3F);
      output[outPos++] = 0x80 | (cp & 0x3F);
    }
    lastWasNewline = false;
    lastWasSpace = false;
  };

  // Alles vor <body> ignorieren.
  const uint8_t* start = input;
  const uint8_t* inputEnd = input + inputLen;
  const char* bodyStart = findTagCI((const char*)input, inputLen, "<body");
  if (bodyStart) {
    const char* bodyTagEnd =
        (const char*)memchr(bodyStart, '>', inputEnd - (const uint8_t*)bodyStart);
    if (bodyTagEnd) start = (const uint8_t*)(bodyTagEnd + 1);
  }
  const uint8_t* end = inputEnd;

  for (const uint8_t* p = start; p < end; p++) {
    char c = (char)*p;

    if (inTag) {
      if (!tagNameDone) {
        if (tagNamePos == 0 && c == '/') { isClosingTag = true; continue; }
        if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
          tagName[tagNamePos] = '\0';
          tagNameDone = true;
        } else if (tagNamePos < (int)sizeof(tagName) - 1) {
          tagName[tagNamePos++] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
        }
      }
      if (c == '>') {
        inTag = false;
        bool isSkipTag = !strcmp(tagName, "head") || !strcmp(tagName, "style") ||
                         !strcmp(tagName, "script");
        if (isSkipTag) skipContent = !isClosingTag;

        if (!skipContent) {
          // Blockelemente erzeugen Zeilenumbrüche.
          static const char* kBlock[] = {"p", "div", "br", "h1", "h2", "h3", "h4",
                                         "h5", "h6", "li", "tr", "blockquote", "hr"};
          for (const char* b : kBlock) {
            if (strcmp(tagName, b) == 0) {
              if (outPos > 0 && !lastWasNewline) {
                output[outPos++] = '\n';
                lastWasNewline = true;
                lastWasSpace = false;
              }
              break;
            }
          }
        }
      }
      continue;
    }

    if (c == '<') {
      inTag = true;
      tagNamePos = 0;
      tagNameDone = false;
      isClosingTag = false;
      continue;
    }
    if (skipContent) continue;

    if (c == '&') {
      uint32_t cp = decodeEntity(p, end, &p);
      if (cp == ' ') {
        if (!lastWasSpace && !lastWasNewline && outPos > 0) {
          output[outPos++] = ' ';
          lastWasSpace = true;
        }
      } else if (cp) {
        emitCp(cp);
      }
      continue;
    }

    // UTF-8-Mehrbyte-Sequenzen dekodieren.
    if ((uint8_t)c >= 0xC0) {
      uint32_t cp = 0;
      int extraBytes = 0;
      if (((uint8_t)c & 0xE0) == 0xC0)      { cp = (uint8_t)c & 0x1F; extraBytes = 1; }
      else if (((uint8_t)c & 0xF0) == 0xE0) { cp = (uint8_t)c & 0x0F; extraBytes = 2; }
      else if (((uint8_t)c & 0xF8) == 0xF0) { cp = (uint8_t)c & 0x07; extraBytes = 3; }

      bool valid = (extraBytes > 0);
      for (int b = 0; b < extraBytes && p + 1 + b < end; b++) {
        uint8_t cb = *(p + 1 + b);
        if ((cb & 0xC0) != 0x80) { valid = false; break; }
        cp = (cp << 6) | (cb & 0x3F);
      }
      if (valid) {
        p += extraBytes;
        emitCp(cp);
      }
      continue;
    }
    if ((uint8_t)c >= 0x80) continue;   // verirrtes Continuation-Byte

    // Whitespace kollabieren.
    if (c == '\n' || c == '\r') {
      if (!lastWasNewline && outPos > 0) {
        output[outPos++] = '\n';
        lastWasNewline = true;
        lastWasSpace = false;
      }
      continue;
    }
    if (c == ' ' || c == '\t') {
      if (!lastWasSpace && !lastWasNewline && outPos > 0) {
        output[outPos++] = ' ';
        lastWasSpace = true;
      }
      continue;
    }

    output[outPos++] = (uint8_t)c;
    lastWasNewline = false;
    lastWasSpace = false;
  }

  while (outPos > 0 && (output[outPos - 1] == '\n' || output[outPos - 1] == ' ')) outPos--;
  output[outPos] = '\0';
  *outLen = outPos;
  return output;
}

void cleanup(bool removeTxt) {
  if (s_cv.zip) { delete s_cv.zip; s_cv.zip = nullptr; }
  freeChapterPaths(s_cv.chapterPaths, s_cv.chapterCount);
  s_cv.chapterPaths = nullptr;
  s_cv.chapterCount = 0;
  spiLock();
  if (s_cv.outFile)  s_cv.outFile.close();
  if (s_cv.epubFile) s_cv.epubFile.close();
  if (removeTxt) SD.remove(s_cv.txtPath);
  spiUnlock();
  // Boost-Gegenstück zu convertBegin(): nur wenn die Konvertierung wirklich
  // lief (die Fehlpfade in convertBegin() räumen vor active=true auf).
  if (s_cv.active) power::boostUnlock();
  s_cv.active = false;
}

}  // namespace

namespace epubproc {

void buildCachePath(const char* epubPath, char* cachePath, int cachePathSize) {
  // Zentraler Cache unter /books/.epub_cache — keine Cache-Ordner in die
  // Calibre-Struktur (Autor/Titel/) streuen. CRC des Pfads macht den Namen
  // auch bei gleichen Dateinamen in verschiedenen Ordnern eindeutig.
  const char* lastSlash = strrchr(epubPath, '/');
  const char* filename = lastSlash ? lastSlash + 1 : epubPath;

  char baseName[80];
  strncpy(baseName, filename, sizeof(baseName) - 1);
  baseName[sizeof(baseName) - 1] = '\0';
  char* dot = strrchr(baseName, '.');
  if (dot) *dot = '\0';

  spiLock();
  if (!SD.exists("/books/.epub_cache")) SD.mkdir("/books/.epub_cache");
  spiUnlock();

  snprintf(cachePath, cachePathSize, "/books/.epub_cache/%08x_%s.txt",
           (unsigned)settings::crc32(epubPath), baseName);
}

bool convertBegin(const char* epubPath, const char* txtPath) {
  if (s_cv.active) return false;
  s_cv = ConvertState{};
  strncpy(s_cv.txtPath, txtPath, sizeof(s_cv.txtPath) - 1);
  s_cv.txtPath[sizeof(s_cv.txtPath) - 1] = '\0';

  spiLock();
  if (SD.exists(txtPath)) {
    spiUnlock();
    s_cv.done = true;
    return true;
  }

  s_cv.epubFile = SD.open(epubPath, FILE_READ);
  if (!s_cv.epubFile) { spiUnlock(); s_cv.failed = true; return false; }

  s_cv.zip = new EpubZipReader();
  if (!s_cv.zip || !s_cv.zip->open(s_cv.epubFile)) {
    spiUnlock();
    Serial.println("[EPUB] ZIP-Struktur nicht lesbar");
    cleanup(false);
    s_cv.failed = true;
    return false;
  }

  char opfPath[kXmlBufSize] = "";
  if (!findOpfPath(s_cv.zip, opfPath, sizeof(opfPath))) {
    spiUnlock();
    Serial.println("[EPUB] OPF nicht gefunden");
    cleanup(false);
    s_cv.failed = true;
    return false;
  }

  char baseDir[kXmlBufSize];
  getDirectory(opfPath, baseDir, sizeof(baseDir));

  char title[128] = "";
  if (!parseOpf(s_cv.zip, opfPath, baseDir, title, sizeof(title),
                &s_cv.chapterPaths, &s_cv.chapterCount)) {
    spiUnlock();
    Serial.println("[EPUB] OPF/Spine nicht parsebar");
    cleanup(false);
    s_cv.failed = true;
    return false;
  }

  s_cv.outFile = SD.open(txtPath, FILE_WRITE);
  if (!s_cv.outFile) {
    spiUnlock();
    cleanup(false);
    s_cv.failed = true;
    return false;
  }
  if (title[0]) {
    s_cv.outFile.println(title);
    s_cv.outFile.println();
  }
  spiUnlock();

  Serial.printf("[EPUB] '%s': %d Kapitel\n", title, s_cv.chapterCount);
  s_cv.active = true;
  power::boostLock();   // XHTML-Strippen ist CPU-Arbeit — voller Takt bis cleanup()
  return true;
}

bool convertStep() {
  if (!s_cv.active || s_cv.done || s_cv.failed) return false;
  if (s_cv.chapterPos >= s_cv.chapterCount) {
    spiLock();
    s_cv.outFile.flush();
    spiUnlock();
    bool ok = (s_cv.written > 0);
    cleanup(!ok);
    s_cv.done = ok;
    s_cv.failed = !ok;
    return false;
  }

  int i = s_cv.chapterPos++;
  uint32_t rawSize = 0;
  uint8_t* rawData = nullptr;
  spiLock();
  int entryIdx = s_cv.zip->findEntry(s_cv.chapterPaths[i]);   // RAM-only
  if (entryIdx >= 0) rawData = s_cv.zip->extractEntry(entryIdx, &rawSize);
  spiUnlock();

  if (rawData && rawSize > 0) {
    // XHTML-Strippen ist reine RAM/CPU-Arbeit — bewusst OHNE spiLock, damit
    // der Audio-Task währenddessen dekodieren kann (Kapitel können groß sein).
    uint32_t textLen = 0;
    uint8_t* plainText = stripXhtml(rawData, rawSize, &textLen);
    free(rawData);
    rawData = nullptr;
    if (plainText && textLen > 0) {
      spiLock();
      s_cv.outFile.write(plainText, textLen);
      s_cv.outFile.print("\n\n");
      spiUnlock();
      s_cv.written++;
    }
    if (plainText) free(plainText);
  }
  if (rawData) free(rawData);
  return true;
}

bool convertDone()   { return s_cv.done; }
bool convertFailed() { return s_cv.failed; }

int convertPercent() {
  if (s_cv.done) return 100;
  if (s_cv.chapterCount <= 0) return 0;
  return (s_cv.chapterPos * 100) / s_cv.chapterCount;
}

void convertCancel() {
  if (s_cv.active) cleanup(true);
  s_cv.done = false;
  s_cv.failed = false;
}

}  // namespace epubproc
