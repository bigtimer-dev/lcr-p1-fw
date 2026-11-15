/* SPDX-License-Identifier: EUPL-1.2 -- LCR-P1: flash is memory mapped, PROGMEM is plain const */
#ifndef COMPAT_AVR_PGMSPACE_H
#define COMPAT_AVR_PGMSPACE_H
#include <stdint.h>
#include <string.h>
#define PROGMEM
#define PSTR(s) (s)
#define pgm_read_byte(a)  (*(const uint8_t *)(a))
#define pgm_read_word(a)  (*(const uint16_t *)(a))
#define pgm_read_dword(a) (*(const uint32_t *)(a))
#define memcpy_P memcpy
#define strcpy_P strcpy
#define strlen_P strlen
#endif
