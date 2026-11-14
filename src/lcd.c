/* SPDX-License-Identifier: EUPL-1.2
 * 1.44" 128x128 ST7735S-type panel (FPC CL14CK269-14B) on SPI: PB13 SCK, PB15 MOSI (AF0),
 * PB4 RST, PB5 DC, PB6 CS. Register values are the ones the stock firmware sends.
 */
#include "lcd.h"
#include "board.h"
#include "hw.h"

#define X_OFS 1     /* panel RAM offset in this orientation (stock: CASET 1..128, RASET 2..129) */
#define Y_OFS 2

static void spi_wait_idle(void)
{
    for (unsigned n = 0; n < 10000; n++)
        if ((SPI_STS & 0x82) == 0x02)           /* transmit empty, not busy */
            return;
}

static inline void spi_byte(uint8_t b)
{
    while (!(SPI_STS & 2))
        ;
    SPI_DAT = b;
}

static void cmd(uint8_t c)
{
    spi_wait_idle();
    gpio_set(PIN_LCD_DC, 0);
    gpio_set(PIN_LCD_CS, 0);
    spi_byte(c);
    spi_wait_idle();
    gpio_set(PIN_LCD_CS, 1);
    gpio_set(PIN_LCD_DC, 1);
}

static void data(const uint8_t *d, unsigned n)
{
    gpio_set(PIN_LCD_CS, 0);
    while (n--)
        spi_byte(*d++);
    spi_wait_idle();
    gpio_set(PIN_LCD_CS, 1);
}

/* init: command, length, data... ; 0xFF = end */
static const uint8_t init_seq[] = {
    0xB1, 3, 0x00, 0x2C, 0x2B,                 /* frame rate */
    0xB2, 3, 0x00, 0x01, 0x01,
    0xB3, 6, 0x00, 0x01, 0x01, 0x00, 0x01, 0x01,
    0xB4, 1, 0x03,                             /* inversion control */
    0xB9, 3, 0xFF, 0x83, 0x47,
    0xC0, 3, 0xA2, 0x02, 0x84,                 /* power */
    0xC1, 1, 0x02,
    0xC2, 2, 0x0A, 0x00,
    0xC3, 2, 0x8A, 0x2A,
    0xC4, 2, 0x8A, 0xEE,
    0xC5, 1, 0x09,                             /* VCOM */
    0x20, 0,                                   /* inversion off */
    0xC7, 1, 0x10,
    0x36, 1, 0x08,
    0xE0, 16, 0x0C, 0x1C, 0x1B, 0x1A, 0x2F, 0x28, 0x20, 0x24, 0x23, 0x22, 0x2A, 0x36, 0x00, 0x05, 0x00, 0x10,
    0xE1, 16, 0x0C, 0x1A, 0x1A, 0x1A, 0x2E, 0x27, 0x21, 0x24, 0x24, 0x22, 0x2A, 0x35, 0x00, 0x05, 0x00, 0x10,
    0x35, 1, 0x00,                             /* tearing effect line on */
    0x3A, 1, 0x05,                             /* 16 bit/pixel */
    0x29, 0,                                   /* display on */
    0x36, 1, 0x68,                             /* orientation: as stock */
    0xFF,
};

void lcd_init(void)
{
    gpio_af(GPIOB, 13, 0);
    gpio_af(GPIOB, 14, 0);
    gpio_af(GPIOB, 15, 0);
    gpio_mode(GPIOB, 13, MODE_AF);
    gpio_mode(GPIOB, 14, MODE_AF);
    gpio_mode(GPIOB, 15, MODE_AF);
    gpio_set(PIN_LCD_CS, 1);
    gpio_set(PIN_LCD_DC, 1);
    gpio_set(PIN_LCD_RST, 1);
    gpio_mode(PIN_LCD_CS, MODE_OUT);
    gpio_mode(PIN_LCD_DC, MODE_OUT);
    gpio_mode(PIN_LCD_RST, MODE_OUT);

    SPI_CR1 = 0xC317;                           /* 1-line TX master, CPOL/CPHA 1, f/8, SW CS */
    SPI_CRCPOLY = 7;
    SPI_I2SCFG = 0;
    SPI_CR1 = 0xC357;                           /* enable */

    delay_ms(1);
    gpio_set(PIN_LCD_RST, 0);
    delay_ms(10);
    gpio_set(PIN_LCD_RST, 1);
    delay_ms(120);
    cmd(0x11);                                  /* sleep out */
    delay_ms(120);
    for (const uint8_t *p = init_seq; *p != 0xFF; p += 2 + p[1]) {
        cmd(p[0]);
        if (p[1])
            data(p + 2, p[1]);
    }
}

static void window(int x, int y, int w, int h)
{
    uint8_t a[4];
    x += X_OFS, y += Y_OFS;
    a[0] = 0, a[1] = x, a[2] = 0, a[3] = x + w - 1;
    cmd(0x2A); data(a, 4);
    a[1] = y, a[3] = y + h - 1;
    cmd(0x2B); data(a, 4);
    cmd(0x2C);
}

void lcd_fill(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0)
        return;
    window(x, y, w, h);
    gpio_set(PIN_LCD_CS, 0);
    for (int n = w * h; n; n--) {
        spi_byte(c >> 8);
        spi_byte(c);
    }
    spi_wait_idle();
    gpio_set(PIN_LCD_CS, 1);
}

static int glyph(int x, int y, unsigned ch, const struct font *f, uint16_t fg, uint16_t bg)
{
    if (ch < f->first || ch >= f->first + f->count)
        ch = '?';
    if (x + f->w > LCD_W || y + f->h > LCD_H || x < 0 || y < 0)
        return f->w;
    const uint8_t *g = f->bits + (ch - f->first) * f->h * f->bpr;
    window(x, y, f->w, f->h);
    gpio_set(PIN_LCD_CS, 0);
    for (int r = 0; r < f->h; r++, g += f->bpr)
        for (int c = 0; c < f->w; c++) {
            uint16_t px = g[c >> 3] & (0x80 >> (c & 7)) ? fg : bg;
            spi_byte(px >> 8);
            spi_byte(px);
        }
    spi_wait_idle();
    gpio_set(PIN_LCD_CS, 1);
    return f->w;
}

int lcd_text(int x, int y, const char *s, const struct font *f, uint16_t fg, uint16_t bg)
{
    while (*s)
        x += glyph(x, y, (uint8_t)*s++, f, fg, bg);
    return x;
}

int lcd_text_width(const char *s, const struct font *f)
{
    int n = 0;
    while (*s++)
        n++;
    return n * f->w;
}
