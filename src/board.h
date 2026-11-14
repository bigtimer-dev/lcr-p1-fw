/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef BOARD_H
#define BOARD_H
#include <stdint.h>

extern volatile uint32_t g_ms;          /* milliseconds since boot (SysTick) */

void clock_init(void);
void power_hold(void);                  /* first thing after reset: keep the meter on */
void board_init(void);
void power_off(void);                   /* releases the latch; does not return */

void gpio_mode(uint32_t port, unsigned pin, unsigned mode);
void gpio_pull(uint32_t port, unsigned pin, unsigned pull);
void gpio_af(uint32_t port, unsigned pin, unsigned af);
void gpio_set(uint32_t port, unsigned pin, int on);
int gpio_get(uint32_t port, unsigned pin);

void delay_ms(uint32_t ms);
void relay(int on);
void beep(unsigned ms);                 /* non-blocking; beep_poll() ends it */
void beep_poll(void);

/* keys: debounced; events for the main loop */
enum { KEY_NONE, KEY_TEST, KEY_TEST_LONG, KEY_ZENER };
int key_poll(void);
int key_test_down(void);
int key_zener_down(void);
int mode_ir(void);

#endif
