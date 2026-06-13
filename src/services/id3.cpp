#include "id3.h"
#include "core/board.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>

namespace {

// Latin-1 -> UTF-8 (für ID3v1 und v2-Encoding 0).
void latin1ToUtf8(const uint8_t* in, size_t inLen, char* out, size_t outLen) {
  size_t o = 0;
  for (size_t i = 0; i < inLen && in[i]; i++) {
    uint8_t c = in[i];
    if (c < 0x80) {
      if (o + 1 >= outLen) break;
      out[o++] = (char)c;
    } else {
      if (o + 2 >= outLen) break;
      out[o++] = (char)(0xC0 | (c >> 6));
      out[o++] = (char)(0x80 | (c & 0x3F));
    }
  }
  out[o] = '\0';
}

// UTF-16 (LE/BE, BMP) -> UTF-8.
void utf16ToUtf8(const uint8_t* in, size_t inLen, bool bigEndian, char* out, size_t outLen) {
  size_t o = 0;
  for (size_t i = 0; i + 1 < inLen; i += 2) {
    uint16_t cp = bigEndian ? ((in[i] << 8) | in[i + 1]) : (in[i] | (in[i + 1] << 8));
    if (cp == 0) break;
    if (cp == 0xFEFF) continue;            // BOM
    if (cp >= 0xD800 && cp <= 0xDFFF) {    // Surrogat-Paar -> überspringen
      i += 2;
      if (o + 1 < outLen) out[o++] = '?';
      continue;
    }
    if (cp < 0x80) {
      if (o + 1 >= outLen) break;
      out[o++] = (char)cp;
    } else if (cp < 0x800) {
      if (o + 2 >= outLen) break;
      out[o++] = (char)(0xC0 | (cp >> 6));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    } else {
      if (o + 3 >= outLen) break;
      out[o++] = (char)(0xE0 | (cp >> 12));
      out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  out[o] = '\0';
}

// Text-Frame-Payload (mit Encoding-Byte) dekodieren.
void decodeText(const uint8_t* buf, size_t len, char* out, size_t outLen) {
  out[0] = '\0';
  if (len < 2) return;
  uint8_t enc = buf[0];
  const uint8_t* p = buf + 1;
  size_t n = len - 1;
  switch (enc) {
    case 0: latin1ToUtf8(p, n, out, outLen); break;                 // ISO-8859-1
    case 1:                                                          // UTF-16 + BOM
      if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) utf16ToUtf8(p + 2, n - 2, true, out, outLen);
      else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) utf16ToUtf8(p + 2, n - 2, false, out, outLen);
      else utf16ToUtf8(p, n, false, out, outLen);
      break;
    case 2: utf16ToUtf8(p, n, true, out, outLen); break;            // UTF-16BE
    case 3: {                                                        // UTF-8
      size_t c = (n < outLen - 1) ? n : outLen - 1;
      memcpy(out, p, c);
      out[c] = '\0';
      break;
    }
    default: break;
  }
  // Trailing-Whitespace entfernen.
  size_t e = strlen(out);
  while (e > 0 && (out[e - 1] == ' ' || out[e - 1] == '\t')) out[--e] = '\0';
}

uint32_t synchsafe(const uint8_t* b) {
  return ((uint32_t)(b[0] & 0x7F) << 21) | ((uint32_t)(b[1] & 0x7F) << 14) |
         ((uint32_t)(b[2] & 0x7F) << 7)  | (b[3] & 0x7F);
}

// ID3v1: letzte 128 Bytes "TAG" + title[30] artist[30] album[30].
bool readV1(File& f, id3::Tags& out) {
  size_t sz = f.size();
  if (sz < 128) return false;
  uint8_t buf[128];
  if (!f.seek(sz - 128) || f.read(buf, 128) != 128) return false;
  if (memcmp(buf, "TAG", 3) != 0) return false;
  latin1ToUtf8(buf + 3,  30, out.title,  sizeof(out.title));
  latin1ToUtf8(buf + 33, 30, out.artist, sizeof(out.artist));
  latin1ToUtf8(buf + 63, 30, out.album,  sizeof(out.album));
  return out.title[0] || out.artist[0] || out.album[0];
}

// ID3v2: Frames durchgehen, Payloads uninteressanter Frames per seek überspringen.
bool readV2(File& f, id3::Tags& out) {
  uint8_t hdr[10];
  if (!f.seek(0) || f.read(hdr, 10) != 10) return false;
  if (memcmp(hdr, "ID3", 3) != 0) return false;

  uint8_t  ver     = hdr[3];                  // 2 / 3 / 4
  uint8_t  flags   = hdr[5];
  uint32_t tagSize = synchsafe(hdr + 6);
  uint32_t tagEnd  = 10 + tagSize;

  uint32_t pos = 10;
  // Extended Header (v2.3/2.4) überspringen.
  if ((flags & 0x40) && ver >= 3) {
    uint8_t eh[4];
    if (f.read(eh, 4) != 4) return false;
    uint32_t ehSize = (ver == 4) ? synchsafe(eh)
                                 : (((uint32_t)eh[0] << 24) | ((uint32_t)eh[1] << 16) |
                                    ((uint32_t)eh[2] << 8) | eh[3]);
    pos += 4 + ehSize - ((ver == 4) ? 4 : 0);  // v2.4: Größe inkl. der 4 Bytes
    if (!f.seek(pos)) return false;
  }

  bool any = false;
  int  remaining = 3;   // TIT2/TPE1/TALB

  while (pos < tagEnd && remaining > 0) {
    if (!f.seek(pos)) break;

    char     id[5] = {0};
    uint32_t fsize = 0;
    uint32_t hsize;

    if (ver == 2) {                       // v2.2: 3-Byte-ID + 3-Byte-Größe
      uint8_t fh[6];
      if (f.read(fh, 6) != 6) break;
      memcpy(id, fh, 3);
      fsize = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
      hsize = 6;
    } else {                              // v2.3/2.4: 4-Byte-ID + Größe + Flags
      uint8_t fh[10];
      if (f.read(fh, 10) != 10) break;
      memcpy(id, fh, 4);
      fsize = (ver == 4) ? synchsafe(fh + 4)
                         : (((uint32_t)fh[4] << 24) | ((uint32_t)fh[5] << 16) |
                            ((uint32_t)fh[6] << 8) | fh[7]);
      hsize = 10;
    }
    if (id[0] == 0 || fsize == 0 || pos + hsize + fsize > tagEnd) break;

    char* dst = nullptr; size_t dstLen = 0;
    if      (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) { dst = out.title;  dstLen = sizeof(out.title); }
    else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) { dst = out.artist; dstLen = sizeof(out.artist); }
    else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) { dst = out.album;  dstLen = sizeof(out.album); }

    if (dst && dst[0] == '\0') {
      // Text-Frame lesen (gekappt auf 256 B — reicht für Titelzeilen).
      uint8_t buf[256];
      size_t  rd = fsize < sizeof(buf) ? fsize : sizeof(buf);
      if (f.read(buf, rd) != rd) break;
      decodeText(buf, rd, dst, dstLen);
      if (dst[0]) { any = true; remaining--; }
    }
    pos += hsize + fsize;                 // uninteressante Payloads: nur seek
  }
  return any;
}

}  // namespace

namespace id3 {

bool read(const char* path, Tags& out) {
  memset(&out, 0, sizeof(out));
  bool ok = false;
  spiLock();
  File f = SD.open(path);
  if (f) {
    out.fsize = (uint32_t)f.size();
    ok = readV2(f, out);
    if (!ok) ok = readV1(f, out);
    f.close();
  }
  spiUnlock();
  return ok;
}

}  // namespace id3
