#pragma once
// =============================================================================
// epubzip.h — minimaler ZIP-Reader für EPUB-Dateien (Port aus
// archive_legacy/examples/companion_radio/ui-new/EpubZipReader.h).
//
// Parst ZIP-Archive direkt aus SD-File-Objekten; DEFLATE via tinfl aus dem
// ESP32-ROM (keine externe Library). STORED + DEFLATED, kein ZIP64.
// Puffer kommen aus dem PSRAM. Aufrufer hält spiLock() um alle Aufrufe!
// =============================================================================

#include <SD.h>
#include <FS.h>

#if __has_include(<rom/miniz.h>)
  #include <rom/miniz.h>
  #define HAS_ROM_TINFL 1
#elif __has_include(<esp32s3/rom/miniz.h>)
  #include <esp32s3/rom/miniz.h>
  #define HAS_ROM_TINFL 1
#elif __has_include(<esp32/rom/miniz.h>)
  #include <esp32/rom/miniz.h>
  #define HAS_ROM_TINFL 1
#else
  #warning "ROM miniz nicht gefunden - DEFLATE-Eintraege werden nicht unterstuetzt"
  #define HAS_ROM_TINFL 0
#endif

#define ZIP_LOCAL_FILE_HEADER_SIG   0x04034b50
#define ZIP_CENTRAL_DIR_SIG         0x02014b50
#define ZIP_END_OF_CENTRAL_DIR_SIG  0x06054b50

#define ZIP_METHOD_STORED   0
#define ZIP_METHOD_DEFLATED 8

#define ZIP_MAX_ENTRIES 128
#define ZIP_MAX_FILENAME 128

struct ZipEntry {
  char     filename[ZIP_MAX_FILENAME];
  uint16_t compressionMethod;
  uint32_t compressedSize;
  uint32_t uncompressedSize;
  uint32_t localHeaderOffset;
  uint32_t crc32;
};

static inline uint16_t zipRead16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t zipRead32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

class EpubZipReader {
public:
  EpubZipReader() : _entries(nullptr), _entryCount(0), _isOpen(false) {
    // Entry-Tabelle (~19 KB) aus dem PSRAM — zu groß für den Task-Stack.
    _entries = (ZipEntry*)ps_malloc(ZIP_MAX_ENTRIES * sizeof(ZipEntry));
    if (!_entries) _entries = (ZipEntry*)malloc(ZIP_MAX_ENTRIES * sizeof(ZipEntry));
  }

  ~EpubZipReader() {
    if (_entries) { free(_entries); _entries = nullptr; }
  }

  // ZIP öffnen und Central Directory parsen.
  bool open(File& zipFile) {
    _isOpen = false;
    _entryCount = 0;
    if (!_entries || !zipFile || !zipFile.available()) return false;

    _file = zipFile;
    uint32_t fileSize = _file.size();
    if (fileSize < 22) return false;

    // End-of-Central-Directory rückwärts suchen (EPUBs haben i. d. R. keinen Kommentar).
    uint32_t searchStart = (fileSize > 1024) ? fileSize - 1024 : 0;
    uint32_t searchLen   = fileSize - searchStart;
    bool foundEocd = false;

    uint8_t* searchBuf = (uint8_t*)_allocBuffer(searchLen);
    if (!searchBuf) return false;
    _file.seek(searchStart);
    if (_file.read(searchBuf, searchLen) != (int)searchLen) { free(searchBuf); return false; }

    for (int i = (int)searchLen - 22; i >= 0; i--) {
      if (zipRead32(&searchBuf[i]) == ZIP_END_OF_CENTRAL_DIR_SIG) {
        _totalEntries = zipRead16(&searchBuf[i + 10]);
        _cdSize       = zipRead32(&searchBuf[i + 12]);
        _cdOffset     = zipRead32(&searchBuf[i + 16]);
        foundEocd = true;
        break;
      }
    }
    free(searchBuf);
    if (!foundEocd) return false;
    if (_cdSize == 0 || _cdSize > 512 * 1024) return false;

    uint8_t* cdBuf = (uint8_t*)_allocBuffer(_cdSize);
    if (!cdBuf) return false;
    _file.seek(_cdOffset);
    if (_file.read(cdBuf, _cdSize) != (int)_cdSize) { free(cdBuf); return false; }

    uint32_t pos = 0;
    _entryCount = 0;
    while (pos + 46 <= _cdSize && _entryCount < ZIP_MAX_ENTRIES) {
      if (zipRead32(&cdBuf[pos]) != ZIP_CENTRAL_DIR_SIG) break;

      uint16_t method      = zipRead16(&cdBuf[pos + 10]);
      uint32_t crc         = zipRead32(&cdBuf[pos + 16]);
      uint32_t compSize    = zipRead32(&cdBuf[pos + 20]);
      uint32_t uncompSize  = zipRead32(&cdBuf[pos + 24]);
      uint16_t fnLen       = zipRead16(&cdBuf[pos + 28]);
      uint16_t extraLen    = zipRead16(&cdBuf[pos + 30]);
      uint16_t commentLen  = zipRead16(&cdBuf[pos + 32]);
      uint32_t localOffset = zipRead32(&cdBuf[pos + 42]);

      int copyLen = (fnLen < ZIP_MAX_FILENAME - 1) ? fnLen : ZIP_MAX_FILENAME - 1;
      memcpy(_entries[_entryCount].filename, &cdBuf[pos + 46], copyLen);
      _entries[_entryCount].filename[copyLen] = '\0';
      _entries[_entryCount].compressionMethod = method;
      _entries[_entryCount].compressedSize    = compSize;
      _entries[_entryCount].uncompressedSize  = uncompSize;
      _entries[_entryCount].localHeaderOffset = localOffset;
      _entries[_entryCount].crc32             = crc;

      // Verzeichnisse (Name endet auf '/') überspringen.
      if (copyLen > 0 && _entries[_entryCount].filename[copyLen - 1] != '/') _entryCount++;

      pos += 46 + fnLen + extraLen + commentLen;
    }
    free(cdBuf);

    _isOpen = true;
    return true;
  }

  void close() { _isOpen = false; _entryCount = 0; }

  int getEntryCount() const { return _entryCount; }

  const ZipEntry* getEntry(int index) const {
    if (index < 0 || index >= _entryCount) return nullptr;
    return &_entries[index];
  }

  int findEntry(const char* filename) const {
    for (int i = 0; i < _entryCount; i++)
      if (strcmp(_entries[i].filename, filename) == 0) return i;
    return -1;
  }

  int findEntryBySuffix(const char* suffix) const {
    int suffixLen = strlen(suffix);
    for (int i = 0; i < _entryCount; i++) {
      int fnLen = strlen(_entries[i].filename);
      if (fnLen >= suffixLen &&
          strcasecmp(&_entries[i].filename[fnLen - suffixLen], suffix) == 0) return i;
    }
    return -1;
  }

  // Eintrag in frisch allozierten Puffer entpacken (Aufrufer gibt frei!).
  uint8_t* extractEntry(int index, uint32_t* outSize) {
    if (!_isOpen || index < 0 || index >= _entryCount) return nullptr;
    const ZipEntry& entry = _entries[index];

    uint8_t localHeader[30];
    _file.seek(entry.localHeaderOffset);
    if (_file.read(localHeader, 30) != 30) return nullptr;
    if (zipRead32(localHeader) != ZIP_LOCAL_FILE_HEADER_SIG) return nullptr;

    uint16_t localFnLen    = zipRead16(&localHeader[26]);
    uint16_t localExtraLen = zipRead16(&localHeader[28]);
    uint32_t dataOffset = entry.localHeaderOffset + 30 + localFnLen + localExtraLen;

    if (entry.compressionMethod == ZIP_METHOD_STORED)
      return _extractStored(dataOffset, entry.uncompressedSize, outSize);
    if (entry.compressionMethod == ZIP_METHOD_DEFLATED)
      return _extractDeflated(dataOffset, entry.compressedSize, entry.uncompressedSize, outSize);
    return nullptr;
  }

  uint8_t* extractByName(const char* filename, uint32_t* outSize) {
    int idx = findEntry(filename);
    if (idx < 0) return nullptr;
    return extractEntry(idx, outSize);
  }

  bool isOpen() const { return _isOpen; }

private:
  File       _file;
  ZipEntry*  _entries;
  int        _entryCount;
  bool       _isOpen;
  uint32_t   _cdOffset = 0;
  uint32_t   _cdSize = 0;
  uint16_t   _totalEntries = 0;

  void* _allocBuffer(size_t size) {
    void* buf = ps_malloc(size);
    if (!buf) buf = malloc(size);
    return buf;
  }

  uint8_t* _extractStored(uint32_t dataOffset, uint32_t size, uint32_t* outSize) {
    uint8_t* buf = (uint8_t*)_allocBuffer(size + 1);
    if (!buf) return nullptr;
    _file.seek(dataOffset);
    if ((uint32_t)_file.read(buf, size) != size) { free(buf); return nullptr; }
    buf[size] = '\0';
    *outSize = size;
    return buf;
  }

  uint8_t* _extractDeflated(uint32_t dataOffset, uint32_t compSize,
                            uint32_t uncompSize, uint32_t* outSize) {
#if HAS_ROM_TINFL
    uint8_t* compBuf = (uint8_t*)_allocBuffer(compSize);
    if (!compBuf) return nullptr;
    uint8_t* outBuf = (uint8_t*)_allocBuffer(uncompSize + 1);
    if (!outBuf) { free(compBuf); return nullptr; }
    // tinfl_decompressor ist ~11 KB — heap-allozieren, nicht auf den Stack!
    tinfl_decompressor* decomp = (tinfl_decompressor*)_allocBuffer(sizeof(tinfl_decompressor));
    if (!decomp) { free(compBuf); free(outBuf); return nullptr; }

    _file.seek(dataOffset);
    if ((uint32_t)_file.read(compBuf, compSize) != compSize) {
      free(decomp); free(compBuf); free(outBuf);
      return nullptr;
    }

    tinfl_init(decomp);
    size_t inBytes = compSize;
    size_t outBytes = uncompSize;
    tinfl_status status = tinfl_decompress(
        decomp, (const mz_uint8*)compBuf, &inBytes, outBuf, outBuf, &outBytes,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);   // rohes DEFLATE, single-shot

    free(decomp);
    free(compBuf);

    if (status != TINFL_STATUS_DONE) { free(outBuf); return nullptr; }

    outBuf[outBytes] = '\0';
    *outSize = (uint32_t)outBytes;
    return outBuf;
#else
    *outSize = 0;
    return nullptr;
#endif
  }
};
