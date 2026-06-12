// Arduino.h — minimaler Host-Shim, damit Adafruit-GFX am PC kompiliert.
#pragma once

#define ARDUINO 10805

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <string>

#include "Print.h"
#include "pgmspace.h"

typedef uint8_t  byte;
typedef bool     boolean;

// Adafruit-GFX kennt diese Arduino-Helfer.
#ifndef _swap_int16_t
#define _swap_int16_t(a, b) { int16_t t = a; a = b; b = t; }
#endif

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef constrain
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif
#ifndef F
#define F(x) (x)
#endif
#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#ifndef radians
#define radians(deg) ((deg) * PI / 180.0)
#endif
#ifndef degrees
#define degrees(rad) ((rad) * 180.0 / PI)
#endif
#ifndef sq
#define sq(x) ((x) * (x))
#endif

// PROGMEM-String-Helfertyp (nur als Zeiger benutzt).
class __FlashStringHelper;

// Minimaler Ersatz für Arduino-String (Adafruit-GFX überlädt getTextBounds darauf).
class String {
  std::string s_;
 public:
  String() {}
  String(const char* p) : s_(p ? p : "") {}
  const char* c_str() const { return s_.c_str(); }
  unsigned    length() const { return (unsigned)s_.size(); }
  char        operator[](int i) const { return s_[(size_t)i]; }
};

static inline unsigned long millis() { return 0; }
static inline void delay(unsigned long) {}
static inline void yield() {}
