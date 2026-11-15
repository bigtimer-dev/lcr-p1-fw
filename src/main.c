/* SPDX-License-Identifier: EUPL-1.2
 * LCR-P1 open firmware, phase 2 step 1: ADC explorer on top of the phase 1 bring-up.
 * Page 0: key / switch / relay state. Page 1 (ZENER switches pages): ADC readings.
 * TEST toggles the relay, hold TEST 1 s = off, 3 min idle = off, low battery = off.
 * The HV boost is never enabled and no probe pin is driven.
 */
#include "adc.h"
#include "board.h"
#include "lcd.h"
#include "uart.h"

#define VERSION   "0.2"
#define IDLE_OFF_MS (3u * 60 * 1000)
#define LOWBAT_MV 3300          /* as stock: switch off at or below this */

static void row(int y, const char *label, const char *value, uint16_t c)
{
    int x = lcd_text(4, y, label, &font16, C_LABEL, C_BLACK);
    x = lcd_text(x < 44 ? 44 : x, y, value, &font16, c, C_BLACK);
    lcd_fill(x, y, LCD_W - x, 16, C_BLACK);
}

static void header(void)
{
    lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    lcd_text((LCD_W - lcd_text_width("LCR-P1", &font24)) / 2, 2, "LCR-P1", &font24, C_ACCENT, C_BLACK);
    lcd_text((LCD_W - lcd_text_width("open fw " VERSION, &font16)) / 2, 26, "open fw " VERSION, &font16,
             C_LABEL, C_BLACK);
    lcd_fill(4, 44, LCD_W - 8, 1, C_DIM);
}

static const struct { const char *name; uint8_t ch; uint8_t mul; } adc_rows[] = {
    {"BAT", ADC_CH_BAT, 2}, {"REF", ADC_CH_REF, 1}, {"HV", ADC_CH_ZENER, 10}, {"P1", ADC_CH_P1, 1},
    {"P2", ADC_CH_P2, 1}, {"P3", ADC_CH_P3, 1}, {"16", 16, 1}, {"17", 17, 1},
};
#define NROWS (sizeof adc_rows / sizeof adc_rows[0])

static void off(const char *why)
{
    relay(0);
    uprintf("power off (%s)\r\n", why);
    lcd_fill(0, 45, LCD_W, LCD_H - 45, C_BLACK);
    lcd_text(28, 64, "power off", &font16, C_ACCENT, C_BLACK);
    lcd_text((LCD_W - lcd_text_width(why, &font16)) / 2, 84, why, &font16, C_LABEL, C_BLACK);
    beep(80);
    delay_ms(600);
    power_off();
}

int main(void)
{
    board_init();
    uart_init();
    uprintf("\r\nlcrp1-fw " VERSION " (ADC explorer)\r\n");
    lcd_init();
    int adc_ok = adc_init();
    uprintf("adc init %d\r\n", adc_ok);
    header();
    beep(40);

    int relay_on = 0, page = 0, redraw = 1, low = 0;
    int last_t = -1, last_z = -1, last_m = -1, last_r = -1;
    uint32_t last_s = ~0u, idle = g_ms;
    uint32_t raw[NROWS];
    char buf[24];

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
            page = !page, redraw = 1;
            beep(20);
            uprintf("key ZENER: page %d\r\n", page);
        }
        if (ev == KEY_TEST_LONG)
            off("key");
        if (g_ms - idle > IDLE_OFF_MS)
            off("idle");

        int t = key_test_down(), z = key_zener_down(), m = mode_ir();
        if (m != last_m)
            uprintf("mode %s\r\n", m ? "IR" : "normal");
        if (redraw) {
            if (page == 0)
                header();
            else
                lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
            last_t = last_z = last_r = -1;
            last_s = ~0u;
            redraw = 0;
        }

        uint32_t s = g_ms / 1000;
        if (page == 0) {
            if (t != last_t)
                row(48, "TEST", t ? "down" : "up", t ? C_ACCENT : C_WHITE);
            if (z != last_z)
                row(64, "ZENR", z ? "down" : "up", z ? C_ACCENT : C_WHITE);
            if (m != last_m || last_s == ~0u)
                row(80, "MODE", m ? "IR" : "normal", C_WHITE);
            if (relay_on != last_r)
                row(96, "RLY", relay_on ? "on (open)" : "off", relay_on ? C_ACCENT : C_WHITE);
        }
        last_t = t, last_z = z, last_m = m, last_r = relay_on;

        if (s != last_s) {                      /* once a second: ADC */
            last_s = s;
            for (unsigned i = 0; i < NROWS; i++)
                raw[i] = adc_raw(adc_rows[i].ch, 32) / 32;
            uint32_t bat = raw[0] * 2 * adc_vref_mv / 4096;
            uprintf("#A t=%u bat=%umV", s, bat);
            for (unsigned i = 0; i < NROWS; i++)
                uprintf(" %s=%u", adc_rows[i].name, raw[i]);
            uprintf("\r\n");
            if (page == 0) {
                fmt(buf, sizeof buf, "%u.%02uV %us", bat / 1000, bat % 1000 / 10, s);
                row(112, "BAT", buf, C_LABEL);
            } else {
                for (unsigned i = 0; i < NROWS; i++) {
                    uint32_t mv = raw[i] * adc_rows[i].mul * adc_vref_mv / 4096;
                    fmt(buf, sizeof buf, "%5u %4u", mv, raw[i]);
                    row(16 * i, adc_rows[i].name, buf, i == 0 ? C_ACCENT : C_WHITE);
                }
            }
            /* low battery: only trusted while the reading is plausible (2.5 .. 5 V) */
            if (bat > 2500 && bat <= LOWBAT_MV && s > 2) {
                if (++low >= 5)
                    off("low battery");
            } else {
                low = 0;
            }
        }

        int c = uart_getc();
        if (c == '?')
            uprintf("#? t=%u test=%d zener=%d ir=%d relay=%d adc=%d\r\n", g_ms, t, z, m, relay_on, adc_ok);
    }
}
