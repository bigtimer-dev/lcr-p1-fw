/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef LCD_H_INCLUDED
#define LCD_H_INCLUDED
#include <stdint.h>
#include "font.h"

#define LCD_W 128
#define LCD_H 128

/* RGB565, red in the high bits (MADCTL 0x68 as stock; 0xB47F shows purple on the meter) */
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_ACCENT 0xB47F      /* purple accent of the patched firmware theme */
#define C_LABEL  0x8C33      /* grey labels */
#define C_DIM    0x4A2A
#define C_RED    0xF800
#define C_GREEN  0x07E0

void lcd_init(void);
void lcd_fill(int x, int y, int w, int h, uint16_t c);
int lcd_text(int x, int y, const char *s, const struct font *f, uint16_t fg, uint16_t bg);
int lcd_text_width(const char *s, const struct font *f);

#endif
