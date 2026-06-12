#include "audio.h"
#include "core/board.h"
#include "config.h"

#include <Arduino.h>
#include <SD.h>
#include <Audio.h>          // schreibfaul1/ESP32-audioI2S
#include <esp_heap_caps.h>
#include <string.h>

namespace {

constexpr int    kPathLen = TRACK_PATH_LEN;
typedef char PathBuf[kPathLen];

Audio          s_audio;
QueueHandle_t  s_cmdQ   = nullptr;
TaskHandle_t   s_task   = nullptr;

// Abspiel-Queue: Pfade im PSRAM + Abspiel-Reihenfolge (für Shuffle).
// s_stage wird im UI-Thread gefüllt und per Commit (unter Mutex) übernommen.
// Die Pfad-Puffer sind bedarfsbasiert wachsende Blocktabellen (Blöcke à
// kQBlock Pfade) — kein Fixlimit, kein Umkopieren beim Wachsen.
constexpr int kQBlock    = 512;
constexpr int kQBlockMax = TRACKS_HARD_MAX / kQBlock;

SemaphoreHandle_t s_qMutex = nullptr;
PathBuf* s_pathBlk[kQBlockMax]  = {nullptr};  // committed (Audio-Task liest)
int      s_pathCap  = 0;
PathBuf* s_stageBlk[kQBlockMax] = {nullptr};  // Staging (UI-Thread schreibt)
int      s_stageCap = 0;
uint16_t* s_order   = nullptr;     // Abspiel-Reihenfolge: Index in die Queue
int      s_orderCap = 0;
int      s_qlen = 0;
int      s_qpos = 0;               // Position in s_order
int      s_stageLen = 0;
audio::Owner s_stageOwner = audio::Owner::None;
char     s_curPath[kPathLen] = "";

inline char* pathAt(PathBuf** tbl, int i) { return tbl[i / kQBlock][i % kQBlock]; }

// Staging-Puffer blockweise nachziehen (nur UI-Thread; der Audio-Task liest
// ausschließlich die committeten Blöcke).
bool stageEnsure(int need) {
  if (need > TRACKS_HARD_MAX) return false;
  while (s_stageCap < need) {
    PathBuf* blk = (PathBuf*)heap_caps_malloc(sizeof(PathBuf) * kQBlock, MALLOC_CAP_SPIRAM);
    if (!blk) blk = (PathBuf*)malloc(sizeof(PathBuf) * kQBlock);
    if (!blk) return false;
    s_stageBlk[s_stageCap / kQBlock] = blk;
    s_stageCap += kQBlock;
  }
  return true;
}

// --- Status-Snapshot (vom Task geschrieben, vom UI gelesen) -----------------
// 32-Bit-aligned -> atomare Lese-/Schreibzugriffe auf ESP32, kein Lock nötig.
volatile bool     s_playing = false;
volatile bool     s_paused  = false;
volatile int      s_pos     = -1;
volatile uint32_t s_posSec  = 0;
volatile uint32_t s_durSec  = 0;
volatile uint8_t  s_volume  = AUDIO_VOL_DEFAULT;
volatile uint8_t  s_owner   = (uint8_t)audio::Owner::None;
volatile bool     s_shuffle = false;
volatile uint8_t  s_repeat  = (uint8_t)audio::Repeat::Off;
volatile uint32_t s_sleepAt = 0;    // millis()-Zeitpunkt; 0 = kein Timer

// EOF-Flag, gesetzt im audio_eof_mp3-Callback (läuft im Audio-Task).
volatile bool     s_eof     = false;

enum class Cmd : uint8_t {
  Play, TogglePause, Next, Prev, Stop, SetVolume, SeekRel, SetShuffle, SetRepeat
};
struct Msg { Cmd cmd; int arg; uint32_t arg2; };

void post(Cmd c, int arg = 0, uint32_t arg2 = 0) {
  if (!s_cmdQ) return;
  Msg m{c, arg, arg2};
  xQueueSend(s_cmdQ, &m, 0);
}

// Zufall fürs Shuffle (Hardware-RNG des ESP32).
uint32_t rnd(uint32_t n) { return esp_random() % n; }

// --- im Audio-Task ----------------------------------------------------------
// Reihenfolge neu aufbauen; der aktuell spielende Eintrag bleibt aktuell.
// Aufrufer hält s_qMutex.
void rebuildOrder(bool shuffle) {
  uint16_t curEntry = (s_qpos >= 0 && s_qpos < s_qlen) ? s_order[s_qpos] : 0xFFFF;
  for (int i = 0; i < s_qlen; i++) s_order[i] = (uint16_t)i;
  if (shuffle && s_qlen > 1) {
    for (int i = s_qlen - 1; i > 0; i--) {
      int j = (int)rnd(i + 1);
      uint16_t t = s_order[i]; s_order[i] = s_order[j]; s_order[j] = t;
    }
  }
  if (curEntry != 0xFFFF) {
    for (int i = 0; i < s_qlen; i++) {
      if (s_order[i] == curEntry) {
        // aktuellen Eintrag an die aktuelle Abspielposition tauschen
        int dst = (s_qpos >= 0 && s_qpos < s_qlen) ? s_qpos : 0;
        uint16_t t = s_order[dst]; s_order[dst] = s_order[i]; s_order[i] = t;
        s_qpos = dst;
        break;
      }
    }
  }
}

// Spielt die aktuelle Queue-Position s_qpos (optional ab startSec).
void startCurrent(uint32_t startSec = 0) {
  xSemaphoreTake(s_qMutex, portMAX_DELAY);
  char p[kPathLen] = "";
  if (s_qpos >= 0 && s_qpos < s_qlen) {
    memcpy(p, pathAt(s_pathBlk, s_order[s_qpos]), kPathLen);
    strncpy(s_curPath, p, kPathLen - 1);
    s_curPath[kPathLen - 1] = '\0';
  }
  xSemaphoreGive(s_qMutex);
  if (!p[0]) return;

  s_eof    = false;
  s_posSec = 0;
  s_durSec = 0;
  board::dacPower(true);

  spiLock();
  bool ok = s_audio.connecttoFS(SD, p);
  spiUnlock();

  if (ok) {
    s_audio.setVolume(s_volume);
    if (startSec > 0) {
      // Start-Offset (Resume): zuverlässig bei CBR-MP3; bei VBR ungenau.
      spiLock();
      s_audio.setAudioPlayPosition(startSec);
      spiUnlock();
      s_posSec = startSec;
    }
    s_pos     = s_qpos;
    s_playing = true;
    s_paused  = false;
  } else {
    board::dacPower(false);
    s_playing = false;
  }
}

// delta-Schritte in der Queue; wrap=false stoppt am Ende (Auto-Advance mit
// Repeat::Off), wrap=true (manuelles Next/Prev) läuft immer rundherum.
void advance(int delta, bool wrap) {
  xSemaphoreTake(s_qMutex, portMAX_DELAY);
  int n = s_qlen;
  bool stopAtEnd = false;
  if (n > 0) {
    int np = s_qpos + delta;
    if (np >= n)      { if (wrap) np = 0;     else stopAtEnd = true; }
    else if (np < 0)  { if (wrap) np = n - 1; else stopAtEnd = true; }
    if (!stopAtEnd) s_qpos = np;
  }
  xSemaphoreGive(s_qMutex);
  if (n <= 0) return;
  if (stopAtEnd) {
    s_playing = false;
    s_paused  = false;
    board::dacPower(false);
  } else {
    startCurrent();
  }
}

void stopTrack() {
  if (s_playing) {
    spiLock();
    s_audio.stopSong();
    spiUnlock();
  }
  s_playing = false;
  s_paused  = false;
  s_sleepAt = 0;
  board::dacPower(false);
}

void doSeekRel(int sec) {
  if (!s_playing) return;
  uint32_t dur = s_durSec;
  int32_t target = (int32_t)s_posSec + sec;
  if (target < 0) target = 0;
  if (dur > 0 && target > (int32_t)dur - 2) target = (int32_t)dur - 2;
  if (target < 0) target = 0;
  spiLock();
  s_audio.setAudioPlayPosition((uint16_t)target);
  spiUnlock();
  s_posSec = (uint32_t)target;
}

void handle(const Msg& m) {
  switch (m.cmd) {
    case Cmd::Play:
      if (s_playing) stopTrack();
      xSemaphoreTake(s_qMutex, portMAX_DELAY);
      // Reihenfolge-Puffer an die neue Queue-Länge anpassen (klein: 2 B/Eintrag).
      if (s_stageLen > s_orderCap) {
        uint16_t* no = (uint16_t*)heap_caps_realloc(s_order, sizeof(uint16_t) * s_stageLen, MALLOC_CAP_SPIRAM);
        if (!no) no = (uint16_t*)realloc(s_order, sizeof(uint16_t) * s_stageLen);
        if (no) { s_order = no; s_orderCap = s_stageLen; }
      }
      // Staging übernehmen: Blocktabellen tauschen, Reihenfolge zurücksetzen.
      for (int b = 0; b < kQBlockMax; b++) { PathBuf* t = s_pathBlk[b]; s_pathBlk[b] = s_stageBlk[b]; s_stageBlk[b] = t; }
      { int t = s_pathCap; s_pathCap = s_stageCap; s_stageCap = t; }
      s_qlen  = (s_stageLen <= s_orderCap) ? s_stageLen : s_orderCap;
      s_owner = (uint8_t)s_stageOwner;
      s_qpos  = (m.arg >= 0 && m.arg < s_qlen) ? m.arg : 0;
      for (int i = 0; i < s_qlen; i++) s_order[i] = (uint16_t)i;
      if (s_shuffle) rebuildOrder(true);
      xSemaphoreGive(s_qMutex);
      startCurrent(m.arg2);
      break;
    case Cmd::Next:        advance(+1, true);  break;
    case Cmd::Prev:        advance(-1, true);  break;
    case Cmd::Stop:
      stopTrack();
      s_owner = (uint8_t)audio::Owner::None;
      s_pos   = -1;
      break;
    case Cmd::TogglePause:
      if (s_playing) {
        s_audio.pauseResume();
        s_paused = !s_paused;
      }
      break;
    case Cmd::SetVolume:
      s_volume = (uint8_t)constrain(m.arg, 0, AUDIO_VOL_MAX);
      s_audio.setVolume(s_volume);
      break;
    case Cmd::SeekRel:
      doSeekRel(m.arg);
      break;
    case Cmd::SetShuffle:
      xSemaphoreTake(s_qMutex, portMAX_DELAY);
      s_shuffle = (m.arg != 0);
      rebuildOrder(s_shuffle);
      xSemaphoreGive(s_qMutex);
      break;
    case Cmd::SetRepeat:
      s_repeat = (uint8_t)m.arg;
      break;
  }
}

void audioTask(void*) {
  uint32_t lastPos = 0;
  for (;;) {
    // 1) Kommandos abarbeiten.
    Msg m;
    while (xQueueReceive(s_cmdQ, &m, 0) == pdTRUE) handle(m);

    // 2) Sleep-Timer prüfen.
    if (s_sleepAt && (int32_t)(millis() - s_sleepAt) >= 0) {
      s_sleepAt = 0;
      stopTrack();
    }

    // 3) Dekodieren, solange aktiv.
    if (s_playing && !s_paused) {
#ifdef AUDIO_DEBUG_GAP
      static uint32_t s_lastLoop = 0, s_maxGap = 0, s_loops = 0, s_report = 0;
      uint32_t tn = millis();
      if (s_lastLoop && tn - s_lastLoop > s_maxGap) s_maxGap = tn - s_lastLoop;
      s_lastLoop = tn; s_loops++;
      if (tn - s_report >= 1000) {
        Serial.printf("[GAP] loops/s=%lu maxGap=%lums\n",
                      (unsigned long)s_loops, (unsigned long)s_maxGap);
        s_loops = 0; s_maxGap = 0; s_report = tn;
      }
#endif
      spiLock();
      s_audio.loop();
      spiUnlock();

      // Position/Dauer ~2x/s aktualisieren.
      uint32_t now = millis();
      if (now - lastPos >= 500) {
        lastPos  = now;
        s_posSec = s_audio.getAudioCurrentTime();
        uint32_t d = s_audio.getAudioFileDuration();
        if (d > 0) s_durSec = d;
      }

      // 4) Track-Ende -> Repeat-Logik.
      if (s_eof) {
        s_eof = false;
        audio::Repeat r = (audio::Repeat)s_repeat;
        if (r == audio::Repeat::One)      startCurrent();
        else if (r == audio::Repeat::All) advance(+1, true);
        else                              advance(+1, false);
      }

      vTaskDelay(1);  // 1 Tick (≈1 ms) yield, Bus für Refresh freigeben
    } else {
      // Idle: auf Kommando warten (blockiert bis 20 ms).
      if (xQueueReceive(s_cmdQ, &m, pdMS_TO_TICKS(20)) == pdTRUE) handle(m);
    }
  }
}

}  // namespace

// --- EOF/Info-Callbacks der Lib (weak, hier definiert) ----------------------
void audio_eof_mp3(const char* /*info*/) { s_eof = true; }
void audio_info(const char* info)        { (void)info; /* Serial.printf("AUDIO: %s\n", info); */ }

namespace audio {

void begin() {
  s_cmdQ   = xQueueCreate(8, sizeof(Msg));
  s_qMutex = xSemaphoreCreateMutex();

  // Erster Staging-Block im PSRAM (512 x 192 B = 96 KB); weitere Blöcke und
  // der committed-Puffer entstehen bedarfsbasiert beim Queue-Aufbau/Commit.
  stageEnsure(kQBlock);

  // I2S an den PCM5102A (BCLK, LRC, DOUT). PCM5102A ist ein "dummer" DAC ohne
  // I2C-Konfiguration — reine I2S-Bespielung genügt.
  s_audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  s_audio.setVolume(s_volume);

  // Großer Eingangspuffer im PSRAM = Read-Ahead. Überbrückt die ~650 ms, in
  // denen ein E-Ink-Refresh den geteilten SPI-Bus für SD-Reads blockiert.
  s_audio.setBufsize(-1, 256 * 1024);

  // Audio-Task auf Core 0 (UI/Refresh läuft auf Core 1). Priorität über dem
  // Arduino-loop-Task, damit der Dekoder nie verhungert.
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 4, &s_task, 0);
}

void queueBegin(Owner owner) {
  s_stageLen   = 0;
  s_stageOwner = owner;
}

bool queueAdd(const char* path) {
  if (!path || !path[0] || !stageEnsure(s_stageLen + 1)) return false;
  char* dst = pathAt(s_stageBlk, s_stageLen);
  strncpy(dst, path, kPathLen - 1);
  dst[kPathLen - 1] = '\0';
  s_stageLen++;
  return true;
}

void queueCommit(int startPos, uint32_t startSec) {
  if (s_stageLen <= 0) return;
  post(Cmd::Play, startPos < 0 ? 0 : startPos, startSec);
}

void currentPath(char* out, size_t n) {
  if (!n) return;
  xSemaphoreTake(s_qMutex, portMAX_DELAY);
  if (s_playing || s_pos >= 0) { strncpy(out, s_curPath, n - 1); out[n - 1] = '\0'; }
  else out[0] = '\0';
  xSemaphoreGive(s_qMutex);
}

void togglePause()              { post(Cmd::TogglePause); }
void next()                     { post(Cmd::Next); }
void prev()                     { post(Cmd::Prev); }
void stop()                     { post(Cmd::Stop); }
void seekRel(int sec)           { post(Cmd::SeekRel, sec); }
void setVolume(uint8_t v)       { post(Cmd::SetVolume, v); }
void volumeUp()                 { post(Cmd::SetVolume, (int)s_volume + 1); }
void volumeDown()               { post(Cmd::SetVolume, (int)s_volume - 1); }
void setShuffle(bool on)        { post(Cmd::SetShuffle, on ? 1 : 0); }
void setRepeat(Repeat r)        { post(Cmd::SetRepeat, (int)r); }

void setSleepTimer(uint16_t minutes) {
  s_sleepAt = minutes ? millis() + (uint32_t)minutes * 60000UL : 0;
}

Status status() {
  Status st;
  st.playing  = s_playing;
  st.paused   = s_paused;
  st.pos      = s_pos;
  st.queueLen = s_qlen;
  st.posSec   = s_posSec;
  st.durSec   = s_durSec;
  st.volume   = s_volume;
  st.owner    = (Owner)s_owner;
  st.shuffle  = s_shuffle;
  st.repeat   = (Repeat)s_repeat;
  uint32_t at = s_sleepAt;
  if (at) {
    uint32_t now = millis();
    st.sleepMin = (at > now) ? (uint16_t)((at - now + 59999) / 60000) : 0;
  } else st.sleepMin = 0;
  return st;
}

}  // namespace audio
