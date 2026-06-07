#pragma once

// =============================================================================
// LauncherScreen — minimal home screen (MECK_SIMPLE_LAUNCHER builds)
//
// A horizontal carousel of four large entries:
//   E-Reader -> MP3 Player -> MeshCore -> Settings
//
// Navigation:
//   swipe left/right or tap left/right third  -> previous/next entry
//   tap centre / Enter                        -> open entry
//   keyboard: a/d or arrow keys cycle, Enter opens
//
// The stock Meck HomeScreen (status pages, shutdown, etc.) stays reachable
// behind the MeshCore entry via UITask::gotoMeshHomeScreen(); q/Esc there
// returns here.
// =============================================================================

#ifdef MECK_SIMPLE_LAUNCHER

#include <Arduino.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/ui/DisplayDriver.h>
#include "UITask.h"

// --- 12x12 icon sprites, MSB-first, 2 bytes/row (same format as homeicons.h),
//     drawn scaled up by drawIconScaled() below ---

// Book (E-Reader)
static const uint8_t lr_icon_book[] PROGMEM = {
  0x7F,0xE0, 0x40,0x20, 0x4F,0x20, 0x40,0x20, 0x4F,0x20, 0x40,0x20,
  0x4F,0x20, 0x40,0x20, 0x40,0x20, 0x7F,0xE0, 0x3F,0xC0, 0x00,0x00,
};

// Beamed music note (MP3 Player)
static const uint8_t lr_icon_note[] PROGMEM = {
  0x00,0x00, 0x1F,0xC0, 0x1F,0xC0, 0x18,0x40, 0x18,0x40, 0x18,0x40,
  0x18,0x40, 0x79,0xE0, 0xFB,0xE0, 0xFB,0xE0, 0x71,0xC0, 0x00,0x00,
};

// Antenna with radio waves (MeshCore)
static const uint8_t lr_icon_antenna[] PROGMEM = {
  0x06,0x00, 0x26,0x40, 0x46,0x20, 0x96,0x90, 0x96,0x90, 0x46,0x20,
  0x26,0x40, 0x06,0x00, 0x06,0x00, 0x06,0x00, 0x0F,0x00, 0x1F,0x80,
};

// Gear (Settings)
static const uint8_t lr_icon_gear[] PROGMEM = {
  0x06,0x00, 0x36,0xC0, 0x7F,0xE0, 0x39,0xC0, 0x70,0xE0, 0xF0,0xF0,
  0xF0,0xF0, 0x70,0xE0, 0x39,0xC0, 0x7F,0xE0, 0x36,0xC0, 0x06,0x00,
};

class LauncherScreen : public UIScreen {
public:
  static const int ITEM_COUNT = 4;
  enum Item : uint8_t { ITEM_READER, ITEM_MP3, ITEM_MESH, ITEM_SETTINGS };

  LauncherScreen(UITask* task) : _task(task), _sel(0) {}

  int render(DisplayDriver& display) override {
    static const struct { const uint8_t* icon; const char* label; } ITEMS[ITEM_COUNT] = {
      { lr_icon_book,    "E-Reader" },
      { lr_icon_note,    "MP3 Player" },
      { lr_icon_antenna, "MeshCore" },
      { lr_icon_gear,    "Settings" },
    };

    display.setColor(DisplayDriver::LIGHT);

    // Edge arrows (tap zones: left/right third scroll, centre opens)
    display.setTextSize(0);
    display.setCursor(2, 56);
    display.print("<");
    display.setCursor(122, 56);
    display.print(">");

    // Large icon: 12x12 sprite scaled x5 = 60 virtual px, centred
    const int scale = 5;
    const int iconSize = 12 * scale;
    drawIconScaled(display, ITEMS[_sel].icon, (128 - iconSize) / 2, 14, scale);

    // Label below the icon
    display.setTextSize(1);
    display.drawTextCentered(64, 86, ITEMS[_sel].label);
    display.setTextSize(0);

    // MeshCore entry: show unread message count
    if (_sel == ITEM_MESH) {
      int unread = _task->getUnreadMsgCount();
      if (unread > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d new", unread);
        display.setColor(DisplayDriver::GREEN);
        display.drawTextCentered(64, 98, buf);
        display.setColor(DisplayDriver::LIGHT);
      }
    }

    // Page dots
    const int dotW = 6, dotGap = 4;
    int dotsX = 64 - (ITEM_COUNT * dotW + (ITEM_COUNT - 1) * dotGap) / 2;
    for (int i = 0; i < ITEM_COUNT; i++) {
      int x = dotsX + i * (dotW + dotGap);
      if (i == _sel) display.fillRect(x, 112, dotW, dotW);
      else           display.drawRect(x, 112, dotW, dotW);
    }

    return 5000;  // static screen — re-rendered on input
  }

  bool handleInput(char c) override {
    if (c == 'a' || c == KEY_LEFT || c == KEY_PREV) {
      _sel = (_sel + ITEM_COUNT - 1) % ITEM_COUNT;
      return true;
    }
    if (c == 'd' || c == KEY_RIGHT || c == KEY_NEXT) {
      _sel = (_sel + 1) % ITEM_COUNT;
      return true;
    }
    if (c == KEY_ENTER || c == KEY_SELECT) {
      open((Item)_sel);
      return true;
    }
    return false;
  }

private:
  void open(Item item) {
    switch (item) {
      case ITEM_READER:   _task->gotoTextReader();       break;
      case ITEM_MP3: {
        #if !defined(HAS_4G_MODEM) || defined(MECK_AUDIO_VARIANT)
        // Lazy-inits the Audio object + player screen (defined in main.cpp)
        extern void meckOpenAudiobookPlayer();
        meckOpenAudiobookPlayer();
        #endif
        break;
      }
      case ITEM_MESH:     _task->gotoMeshHomeScreen();   break;
      case ITEM_SETTINGS: _task->gotoSettingsScreen();   break;
    }
  }

  // Blit a 12x12 MSB-first sprite scaled up, one fillRect per set pixel.
  static void drawIconScaled(DisplayDriver& display, const uint8_t* icon,
                             int x, int y, int scale) {
    for (int row = 0; row < 12; row++) {
      uint16_t bits = ((uint16_t)pgm_read_byte(icon + row * 2) << 8) |
                       pgm_read_byte(icon + row * 2 + 1);
      for (int col = 0; col < 12; col++) {
        if (bits & (0x8000 >> col)) {
          display.fillRect(x + col * scale, y + row * scale, scale, scale);
        }
      }
    }
  }

  UITask* _task;
  int _sel;
};

#endif // MECK_SIMPLE_LAUNCHER
