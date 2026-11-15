/* SPDX-License-Identifier: EUPL-1.2
 * LCR-P1 open firmware, phase 1: board bring-up.
 * Shows key / switch / relay state, beeps, powers off on a long TEST press or after 3 min idle,
 * and logs events on the USB serial port. No measurement yet; the HV boost is never enabled.
 */
#include "board.h"
#include "lcd.h"
#include "uart.h"

#define VERSION   "0.1"
#define IDLE_OFF_MS (3u * 60 * 1000)

static void row(int y, const char *label, const char *value, uint16_t c)
{
    lcd_text(4, y, label, &font16, C_LABEL, C_BLACK);
    int x = lcd_text(52, y, value, &font16, c, C_BLACK);
    lcd_fill(x, y, LCD_W - x, 16, C_BLACK);
}

int main(void)
{
    board_init();
    uart_init();
    uprintf("\r\nlcrp1-fw " VERSION " phase 1 (bring-up)\r\n");
    lcd_init();
    lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    lcd_text((LCD_W - lcd_text_width("LCR-P1", &font24)) / 2, 2, "LCR-P1", &font24, C_ACCENT, C_BLACK);
    lcd_text((LCD_W - lcd_text_width("open fw " VERSION, &font16)) / 2, 26, "open fw " VERSION, &font16,
             C_LABEL, C_BLACK);
    lcd_fill(4, 44, LCD_W - 8, 1, C_DIM);
    beep(40);

    int relay_on = 0;
    int last_t = -1, last_z = -1, last_m = -1, last_r = -1;
    uint32_t last_s = ~0u, idle = g_ms;
    char buf[16];

    for (;;) {
        wdt_feed();
        beep_poll();
        int ev = key_poll();
        if (ev != KEY_NONE)
            idle = g_ms;
        if (ev == KEY_TEST) {
            relay_on = !relay_on;
            relay(relay_on);
            beep(30);
            uprintf("key TEST: relay %s\r\n", relay_on ? "on" : "off");
        } else if (ev == KEY_ZENER) {
            beep(30);
            uprintf("key ZENER\r\n");
        }
        if (ev == KEY_TEST_LONG || g_ms - idle > IDLE_OFF_MS) {
            relay(0);
            uprintf(ev == KEY_TEST_LONG ? "power off (key)\r\n" : "power off (idle)\r\n");
            lcd_fill(0, 45, LCD_W, LCD_H - 45, C_BLACK);
            lcd_text(28, 70, "power off", &font16, C_ACCENT, C_BLACK);
            beep(80);
            delay_ms(300);
            power_off();
        }

        int t = key_test_down(), z = key_zener_down(), m = mode_ir();
        if (t != last_t)
            row(48, "TEST", t ? "down" : "up", t ? C_ACCENT : C_WHITE), last_t = t;
        if (z != last_z)
            row(64, "ZENR", z ? "down" : "up", z ? C_ACCENT : C_WHITE), last_z = z;
        if (m != last_m) {
            row(80, "MODE", m ? "IR" : "normal", C_WHITE), last_m = m;
            uprintf("mode %s\r\n", m ? "IR" : "normal");
        }
        if (relay_on != last_r)
            row(96, "RLY", relay_on ? "on (open)" : "off", relay_on ? C_ACCENT : C_WHITE), last_r = relay_on;
        uint32_t s = g_ms / 1000;
        if (s != last_s) {
            fmt(buf, sizeof buf, "%u s", s);
            row(112, "UP", buf, C_LABEL);
            last_s = s;
        }

        int c = uart_getc();
        if (c == '?')
            uprintf("#? t=%u test=%d zener=%d ir=%d relay=%d\r\n", g_ms, t, z, m, relay_on);
    }
}
