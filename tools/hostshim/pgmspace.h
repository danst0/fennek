// pgmspace.h — Host-Shim: PROGMEM ist auf dem PC normaler Speicher.
#pragma once
#include <stdint.h>

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PGM_P
#define PGM_P const char*
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(a)    (*(const uint8_t*)(a))
#endif
#ifndef pgm_read_word
#define pgm_read_word(a)    (*(const uint16_t*)(a))
#endif
#ifndef pgm_read_dword
#define pgm_read_dword(a)   (*(const uint32_t*)(a))
#endif
#ifndef pgm_read_pointer
#define pgm_read_pointer(a) (*(void* const*)(a))
#endif
#define pgm_read_byte_near(a) pgm_read_byte(a)
#define pgm_read_word_near(a) pgm_read_word(a)
#define memcpy_P memcpy
#define strlen_P strlen
#define strcpy_P strcpy
