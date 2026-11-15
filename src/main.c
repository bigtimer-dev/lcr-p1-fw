/* SPDX-License-Identifier: EUPL-1.2
 * LCR-P1 open firmware, phase 2 step 2: ComponentTester probing (resistors so far).
 * Page 0: TEST runs a probing cycle and shows the result. Page 1 (ZENER switches pages): ADC readings.
 * Hold TEST 1 s = off, 3 min idle = off, low battery = off. The HV boost is never enabled.
 */
#include "adc.h"
#include "ct_hal.h"
#include "measure.h"
#include "board.h"
#include "lcd.h"
#include "uart.h"

#define VERSION   "0.3"
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

static void show_result(int found, uint32_t ms)
{
    char v[16], line[24];
    lcd_fill(0, 45, LCD_W, LCD_H - 45, C_BLACK);
    uprintf("#R found=%d vcc=%u ref=%d ms=%u\r\n", found, measure_vcc(), measure_ref_ok(), ms);
    int n = measure_resistors();
    if (n) {
        lcd_text(4, 48, n > 1 ? "Resistors" : "Resistor", &font16, C_LABEL, C_BLACK);
        for (int i = 0; i < n; i++) {
            uint8_t a, b;
            uint32_t value;
            int8_t scale;
            measure_resistor(i, &a, &b, &value, &scale);
            measure_format(v, sizeof v, value, scale, CH_OHM);
            fmt(line, sizeof line, "%u-%u", a, b);
            lcd_text(4, 68 + 26 * i, line, &font16, C_LABEL, C_BLACK);
            lcd_text(36, 64 + 26 * i, v, &font24, C_ACCENT, C_BLACK);
            uprintf("#R R %u-%u %u e%d\r\n", a, b, value, scale);
        }
    } else {
        lcd_text(4, 56, found == 1 ? "error" : "no component", &font16, C_WHITE, C_BLACK);
        lcd_text(4, 76, "(R only so far)", &font16, C_DIM, C_BLACK);
    }
    fmt(line, sizeof line, "Vcc %u.%03uV%s", measure_vcc() / 1000, measure_vcc() % 1000, measure_ref_ok() ? "" : "?");
    lcd_text(4, 112, line, &font16, C_DIM, C_BLACK);
}

int main(void)
{
    board_init();
    uart_init();
    uprintf("\r\nlcrp1-fw " VERSION " (ADC explorer)\r\n");
    lcd_init();
    int adc_ok = adc_init();
    uprintf("adc init %d\r\n", adc_ok);
    ct_hal_init();
    measure_init();
    header();
    beep(40);

    int relay_on = 0, page = 0, redraw = 1, low = 0, shown = 0, c_test = 0;
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
        if (ev == KEY_TEST || c_test) {
            c_test = 0;                         /* no beep here: the test blocks the loop, it would last */
            if (page)
                page = 0, redraw = 1, header();
            lcd_fill(0, 45, LCD_W, LCD_H - 45, C_BLACK);
            lcd_text(4, 56, "probing...", &font16, C_LABEL, C_BLACK);
            uint32_t t0 = g_ms;
            int found = measure_run();
            show_result(found, g_ms - t0);
            redraw = 0, last_t = last_z = last_r = last_m = -2;     /* keep the result on screen */
            shown = 1;
            beep(found ? 60 : 20);
        } else if (ev == KEY_ZENER) {
            page = !page, redraw = 1, shown = 0;
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
        if (page == 0 && !shown) {
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
            if (page == 0 && !shown) {
                fmt(buf, sizeof buf, "%u.%02uV %us", bat / 1000, bat % 1000 / 10, s);
                row(112, "BAT", buf, C_LABEL);
            } else if (page == 1) {
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
        if (c == 'T')
            c_test = 1;                         /* serial: run a test */
        if (c == '?')
            uprintf("#? t=%u test=%d zener=%d ir=%d relay=%d adc=%d\r\n", g_ms, t, z, m, relay_on, adc_ok);
    }
}
