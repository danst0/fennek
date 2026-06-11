// ============================================================================
// BookThumbnail.h — cover JPEG decoding + OPF metadata helpers for the reader
//
// Self-contained so it can be included by Textreaderscreen.h without depending
// on the audiobook player.  All symbols are prefixed `reader`/`READER_` so they
// never collide with the audiobook player's identically-shaped cover helpers,
// which compile into the same translation unit (UITask.cpp).
//
//   readerDecodeJpegThumb()  JPEG bytes  -> 1-bit Bayer-dithered XBM bitmap
//   readerExtractTag()       OPF/XML     -> text content of a <dc:*> element
//   readerExtractAttr()      OPF/XML     -> value of an attribute on a tag
//
// Dependencies: JPEGDEC (bitbank2) — already a project lib_dep.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <JPEGDEC.h>

// Thumbnail size in virtual (128-unit) canvas coordinates.  Portrait-ish so a
// typical book cover keeps its shape; ~2 small-font rows tall.  On the T5S3
// panel these map to roughly 82x76 px (scale_x 7.5, scale_y 4.22).
#ifndef READER_THUMB_W
#define READER_THUMB_W 12
#endif
#ifndef READER_THUMB_H
#define READER_THUMB_H 18
#endif

// 4x4 Bayer ordered-dithering matrix (threshold values 0-255).
static const uint8_t READER_BAYER4x4[4][4] = {
  {  15, 135,  45, 165 },
  { 195,  75, 225, 105 },
  {  60, 180,  30, 150 },
  { 240, 120, 210,  90 }
};

// JPEGDEC draw-callback context: a center-cropped window of the scaled image is
// dithered straight into a 1-bit XBM bitmap.
struct ReaderCoverCtx {
  uint8_t* bitmap;
  int      bitmapW;
  int      bitmapH;
  int      offsetX;   // crop offset into the scaled source
  int      offsetY;
};

static int readerCoverDrawCallback(JPEGDRAW* pDraw) {
  ReaderCoverCtx* ctx = (ReaderCoverCtx*)pDraw->pUser;
  if (!ctx || !ctx->bitmap) return 1;
  int rowBytes = (ctx->bitmapW + 7) / 8;

  for (int y = 0; y < pDraw->iHeight; y++) {
    int destY = pDraw->y + y - ctx->offsetY;
    if (destY < 0 || destY >= ctx->bitmapH) continue;

    for (int x = 0; x < pDraw->iWidth; x++) {
      int destX = pDraw->x + x - ctx->offsetX;
      if (destX < 0 || destX >= ctx->bitmapW) continue;

      uint16_t rgb565 = pDraw->pPixels[y * pDraw->iWidth + x];
      uint8_t r = (rgb565 >> 11) << 3;
      uint8_t g = ((rgb565 >> 5) & 0x3F) << 2;
      uint8_t b = (rgb565 & 0x1F) << 3;
      uint8_t gray = (uint8_t)(((uint16_t)r * 77 + (uint16_t)g * 150 + (uint16_t)b * 29) >> 8);

      uint8_t threshold = READER_BAYER4x4[destY & 3][destX & 3];
      if (gray < threshold) {
        ctx->bitmap[destY * rowBytes + (destX / 8)] |= (0x80 >> (destX & 7));
      }
    }
  }
  return 1;
}

// Decode JPEG bytes into a freshly-allocated dstW x dstH 1-bit XBM bitmap
// (center-cropped to fill, Bayer-dithered).  Returns nullptr on failure; the
// caller owns the returned buffer and must free() it.
inline uint8_t* readerDecodeJpegThumb(const uint8_t* jpegData, uint32_t jpegSize,
                                      int dstW, int dstH) {
  if (!jpegData || jpegSize == 0) return nullptr;

  int bitmapBytes = ((dstW + 7) / 8) * dstH;
  uint8_t* bitmap = (uint8_t*)ps_calloc(1, bitmapBytes);
  if (!bitmap) return nullptr;

  JPEGDEC* jpeg = new JPEGDEC();
  if (!jpeg) { free(bitmap); return nullptr; }

  if (!jpeg->openRAM((uint8_t*)jpegData, jpegSize, readerCoverDrawCallback)) {
    delete jpeg;
    free(bitmap);
    return nullptr;
  }

  int srcW = jpeg->getWidth();
  int srcH = jpeg->getHeight();

  // Pick the largest power-of-two downscale that still covers the thumbnail,
  // so the dithered result stays sharp without wasting decode time.
  int scale = 0;
  if (srcW > dstW * 6 || srcH > dstH * 6) scale = 3;
  else if (srcW > dstW * 3 || srcH > dstH * 3) scale = 2;
  else if (srcW > dstW * 1.5 || srcH > dstH * 1.5) scale = 1;

  int divider = 1 << scale;
  int scaledW = srcW / divider;
  int scaledH = srcH / divider;

  ReaderCoverCtx ctx;
  ctx.bitmap  = bitmap;
  ctx.bitmapW = dstW;
  ctx.bitmapH = dstH;
  ctx.offsetX = (scaledW > dstW) ? (scaledW - dstW) / 2 : 0;
  ctx.offsetY = (scaledH > dstH) ? (scaledH - dstH) / 2 : 0;

  jpeg->setUserPointer(&ctx);
  jpeg->setPixelType(RGB565_BIG_ENDIAN);

  int scaleFlags[] = { JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH };
  int ok = (scale > 0) ? jpeg->decode(0, 0, scaleFlags[scale - 1])
                       : jpeg->decode(0, 0, 0);

  jpeg->close();
  delete jpeg;

  if (!ok) { free(bitmap); return nullptr; }
  return bitmap;
}

// Extract the text content of the first <tag ...>text</tag> element.  Matches
// the tag name case-insensitively and tolerates attributes on the open tag.
// Returns true and writes a NUL-terminated string into `out` on success.
inline bool readerExtractTag(const char* xml, uint32_t len, const char* tag,
                             char* out, int outSize) {
  if (!xml || !tag || outSize <= 0) return false;
  out[0] = '\0';
  int tagLen = strlen(tag);

  for (uint32_t i = 0; i + tagLen + 1 < len; i++) {
    if (xml[i] != '<') continue;
    if (strncasecmp(&xml[i + 1], tag, tagLen) != 0) continue;
    char after = xml[i + 1 + tagLen];
    if (after != '>' && after != ' ' && after != '\t' && after != '\n'
        && after != '\r' && after != '/') continue;

    // Find end of the opening tag '>'
    uint32_t p = i + 1 + tagLen;
    while (p < len && xml[p] != '>') p++;
    if (p >= len || xml[p - 1] == '/') return false;  // self-closed, no text
    p++;  // move past '>'

    // Copy content up to the closing '<'
    int o = 0;
    while (p < len && xml[p] != '<' && o < outSize - 1) {
      out[o++] = xml[p++];
    }
    out[o] = '\0';

    // Trim surrounding whitespace
    int start = 0;
    while (out[start] == ' ' || out[start] == '\n' || out[start] == '\r'
           || out[start] == '\t') start++;
    int end = strlen(out);
    while (end > start && (out[end - 1] == ' ' || out[end - 1] == '\n'
           || out[end - 1] == '\r' || out[end - 1] == '\t')) end--;
    if (start > 0 || end < (int)strlen(out)) {
      memmove(out, out + start, end - start);
      out[end - start] = '\0';
    }
    return out[0] != '\0';
  }
  return false;
}

// Find the value of `attr="..."` that appears on the same tag as a given
// substring `needle` (e.g. a manifest <item> whose id is the cover).  Searches
// the whole buffer for `needle`, then reads the requested attribute on that
// element.  Returns true on success.
inline bool readerExtractAttrOnTagWith(const char* xml, uint32_t len,
                                       const char* needle, const char* attr,
                                       char* out, int outSize) {
  if (!xml || !needle || !attr || outSize <= 0) return false;
  out[0] = '\0';
  const char* hit = strstr(xml, needle);
  if (!hit) return false;

  // Back up to the start of this tag '<'
  const char* tagStart = hit;
  while (tagStart > xml && *tagStart != '<') tagStart--;
  // Forward to the end of this tag '>'
  const char* tagEnd = hit;
  const char* xmlEnd = xml + len;
  while (tagEnd < xmlEnd && *tagEnd != '>') tagEnd++;

  // Search for attr= within [tagStart, tagEnd]
  int attrLen = strlen(attr);
  for (const char* p = tagStart; p + attrLen + 2 < tagEnd; p++) {
    if (strncasecmp(p, attr, attrLen) != 0) continue;
    const char* q = p + attrLen;
    while (q < tagEnd && (*q == ' ' || *q == '\t')) q++;
    if (q >= tagEnd || *q != '=') continue;
    q++;
    while (q < tagEnd && (*q == ' ' || *q == '\t')) q++;
    char quote = *q;
    if (quote != '"' && quote != '\'') continue;
    q++;
    int o = 0;
    while (q < tagEnd && *q != quote && o < outSize - 1) out[o++] = *q++;
    out[o] = '\0';
    return out[0] != '\0';
  }
  return false;
}
