/* SPDX-License-Identifier: EUPL-1.2 -- LCR-P1: watchdog is src/board.c */
#ifndef COMPAT_AVR_WDT_H
#define COMPAT_AVR_WDT_H
void wdt_feed(void);
#define wdt_reset() wdt_feed()
#define wdt_enable(t) ((void)(t))
#define wdt_disable() ((void)0)
#endif
