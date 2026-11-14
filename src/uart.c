/* SPDX-License-Identifier: EUPL-1.2
 * USART1 115200 8N1 on PA9 (TX) / PA10 (RX), AF4, to the board's CH340 (USB-C).
 */
#include <stdarg.h>
#include "uart.h"
#include "board.h"
#include "hw.h"

void uart_init(void)
{
    gpio_af(GPIOA, 9, 4);
    gpio_af(GPIOA, 10, 4);
    gpio_mode(GPIOA, 9, MODE_AF);
    gpio_mode(GPIOA, 10, MODE_AF);
    UART_CR2 = 0;
    UART_CR3 = 0;
    UART_BRR = 417;                             /* 48 MHz / 115200 */
    UART_CR1 = 0x200C;                          /* enable, TX, RX */
}

void uart_putc(char c)
{
    for (unsigned n = 0; !(UART_STS & 0x80) && n < 100000; n++)
        ;
    UART_DAT = (uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

int uart_getc(void)
{
    return UART_STS & 0x20 ? (int)(UART_DAT & 0xFF) : -1;
}

struct sink { char *p; int left; };

static void put(struct sink *k, char c)
{
    if (!k->p)
        uart_putc(c);
    else if (k->left > 1)
        *k->p++ = c, k->left--;
}

static void vfmt(struct sink *k, const char *f, va_list ap)
{
    char buf[12];
    for (; *f; f++) {
        if (*f != '%') {
            put(k, *f);
            continue;
        }
        char pad = ' ';
        int width = 0;
        if (*++f == '0')
            pad = '0', f++;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + *f++ - '0';
        const char *s = buf;
        int n = 0;
        unsigned v;
        switch (*f) {
        case 'c':
            buf[0] = (char)va_arg(ap, int), n = 1;
            break;
        case 's':
            s = va_arg(ap, const char *);
            while (s[n])
                n++;
            break;
        case 'd': case 'u': case 'x': {
            int base = *f == 'x' ? 16 : 10, neg = 0;
            int iv = va_arg(ap, int);
            v = (unsigned)iv;
            if (*f == 'd' && iv < 0)
                neg = 1, v = -(unsigned)iv;
            char *e = buf + sizeof buf, *p = e;
            do
                *--p = "0123456789abcdef"[v % base];
            while (v /= base);
            if (neg)
                *--p = '-';
            s = p, n = e - p;
            break;
        }
        case '%':
            buf[0] = '%', n = 1;
            break;
        default:
            continue;
        }
        while (width-- > n)
            put(k, pad);
        while (n--)
            put(k, *s++);
    }
}

void uprintf(const char *f, ...)
{
    struct sink k = {0, 0};
    va_list ap;
    va_start(ap, f);
    vfmt(&k, f, ap);
    va_end(ap);
}

int fmt(char *out, int size, const char *f, ...)
{
    struct sink k = {out, size};
    va_list ap;
    va_start(ap, f);
    vfmt(&k, f, ap);
    va_end(ap);
    *k.p = 0;
    return size - k.left;
}
