/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef UART_H
#define UART_H

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
int uart_getc(void);                    /* -1 if nothing received */
/* tiny printf: %d %u %x %s %c, optional width with 0 / space padding */
void uprintf(const char *fmt, ...);
int fmt(char *out, int size, const char *f, ...);

#endif
