/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef FONT_H
#define FONT_H
#include <stdint.h>

struct font {
    uint8_t w, h, bpr, first, count;
    const uint8_t *bits;            /* count glyphs x h rows x bpr bytes, MSB = leftmost pixel */
};
extern const struct font font16, font24;   /* Terminus 8x16 normal, 12x24 bold */

/* extra glyphs after '~' */
#define CH_OHM    "\x7f"
#define CH_MICRO  "\x80"
#define CH_DEGREE "\x81"

#endif
