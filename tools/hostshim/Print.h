// Print.h — Host-Shim der Arduino-Print-Basisklasse (genug für Adafruit-GFX).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t* b, size_t n) {
    size_t c = 0;
    while (n--) c += write(*b++);
    return c;
  }
  size_t write(const char* s) { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
  size_t print(const char* s) { return write(s); }
  size_t print(char c)        { return write((uint8_t)c); }
  size_t print(int n)           { char b[16]; return write((const uint8_t*)b, snprintf(b, sizeof b, "%d",  n)); }
  size_t print(unsigned n)      { char b[16]; return write((const uint8_t*)b, snprintf(b, sizeof b, "%u",  n)); }
  size_t print(long n)          { char b[24]; return write((const uint8_t*)b, snprintf(b, sizeof b, "%ld", n)); }
  size_t print(unsigned long n) { char b[24]; return write((const uint8_t*)b, snprintf(b, sizeof b, "%lu", n)); }
};
