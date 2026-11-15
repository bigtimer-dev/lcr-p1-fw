/* SPDX-License-Identifier: EUPL-1.2 -- LCR-P1: EEPROM data lives in flash (read only for now) */
#ifndef COMPAT_AVR_EEPROM_H
#define COMPAT_AVR_EEPROM_H
#include <stdint.h>
#include <string.h>
#define EEMEM
#define eeprom_read_byte(a)  (*(const uint8_t *)(a))
#define eeprom_read_word(a)  (*(const uint16_t *)(a))
#define eeprom_read_block(dst, src, n) memcpy((dst), (src), (n))
/* writes: persisted later by the LCR-P1 NV store; ignored for now */
#define eeprom_update_byte(a, v)  ((void)(a), (void)(v))
#define eeprom_update_word(a, v)  ((void)(a), (void)(v))
#define eeprom_update_block(src, dst, n) ((void)(src), (void)(dst), (void)(n))
#define eeprom_write_byte eeprom_update_byte
#define eeprom_write_word eeprom_update_word
#define eeprom_write_block eeprom_update_block
#endif
