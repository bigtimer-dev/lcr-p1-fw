/* SPDX-License-Identifier: EUPL-1.2
 * LCR-P1 open firmware, phase 2 step 2: ComponentTester probing (resistors so far).
 * Page 0: TEST runs a probing cycle and shows the result. Page 1 (ZENER switches pages): ADC readings.
 * Hold TEST 1 s = off, 3 min idle = off, low battery = off. The HV boost is never enabled.
 */
#include "adc.h"
#include "hw.h"
#include "ct_hal.h"
#include "measure.h"
#include "board.h"
#include "lcd.h"
#include "uart.h"

#define VERSION   "0.4"
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

/* serial 'D': probe network diagnostic. For each probe: drive it high via Rl, Rh or directly while the
   other two are pulled low directly; print the three probe voltages (raw, 8 samples) and the GPIO state. */
static void probe_diag(void)
{
    static const char *how[3] = {"Rl", "Rh", "dir"};
    ADC_DDR_set(0);                             /* relay on: probes free */
    MilliSleep(50);
    uprintf("#D relay PB3=%d\r\n", (int)(R32(GPIOB + 0x14) >> 3 & 1));
    for (int p = 0; p < 3; p++)
        for (int m = 0; m < 3; m++) {
            uint8_t others = (uint8_t)(7 & ~(1u << p));
            R_DDR_set(0);
            R_PORT_set(0);
            ADC_PORT_set(m == 2 ? (uint8_t)(1u << p) : 0);
            ADC_DDR_set(m == 2 ? 7 : others);
            if (m < 2) {
                uint8_t bit = (uint8_t)(1u << (2 * p + m));
                R_PORT_set(bit);
                R_DDR_set(bit);
            }
            MilliSleep(5);
            uprintf("#D P%d %s: %u %u %u  A mode=%x out=%x  B mode=%x out=%x\r\n", p + 1, how[m],
                    adc_raw(1, 8) / 8, adc_raw(3, 8) / 8, adc_raw(6, 8) / 8,
                    R32(GPIOA), R32(GPIOA + 0x14), R32(GPIOB), R32(GPIOB + 0x14));
        }
    R_DDR_set(0);
    R_PORT_set(0);
    ADC_PORT_set(0);
    ADC_DDR_set(1 << 4);                        /* probes shorted again */
    uprintf("#D end\r\n");
}

/* serial 'X': why do the probe pins drive high only to ~87 % of the ADC full scale? Drive probe 1
   (PA1) high with probes 2/3 floating and read it; then apply, one at a time, the port A values the
   stock firmware writes (output type, +0x08, drive strength +0x2C) and read again. */
/* #G: FET switching trace with G = probe 1 (Rh up), D = probe 2 (Rl up), S = probe 3 (low).
   Gate first discharged via Rl for 10 ms; then gate/drain sampled with the fast poll read. */
static void gate_trace(void)
{
    static uint16_t tg[80], vg[80], vd[80];
    ADC_DDR_set(0);                             /* relay on: probes free */
    MilliSleep(20);
    ADC_PORT_set(0);
    ADC_DDR_set(1 << 2);                        /* source (probe 3) low directly */
    R_PORT_set(1 << 2);                         /* drain Rl (probe 2) high */
    R_DDR_set(1 << 2 | 1 << 0);                 /* + gate Rl (probe 1) low: discharge */
    MilliSleep(10);
    R_PORT_set(1 << 2 | 1 << 1);                /* gate via Rh high */
    R_DDR_set(1 << 2 | 1 << 1);
    uint16_t t0 = ct_us();
    for (int i = 0; i < 80; i++) {
        tg[i] = (uint16_t)(ct_us() - t0);
        vd[i] = ct_adc_poll(1);
        vg[i] = ct_adc_poll(0);
        for (uint16_t w = ct_us(); (uint16_t)(ct_us() - w) < 40; )
            ;
    }
    R_DDR_set(1 << 2);                          /* gate HiZ */
    uint16_t hold = (uint16_t)ct_adc_read(0, 1);
    R_DDR_set(0);
    R_PORT_set(0);
    ADC_DDR_set(1 << 4);                        /* probes shorted again */
    for (int i = 0; i < 80; i++)
        uprintf("#G %u us G=%u D=%u\r\n", tg[i], vg[i] * 4993u / 4096, vd[i] * 4993u / 4096);
    uprintf("#G gate after HiZ (code %u) = %u mV\r\n", adc_sample_code, hold * 4993u / 4096);
}

static void drive_experiment(void)
{
    ADC_DDR_set(0);
    MilliSleep(30);
    R_DDR_set(0);
    ADC_PORT_set(1);
    ADC_DDR_set(1);                             /* PA1 output high, everything else floating */
    MilliSleep(5);
    uint32_t o4 = R32(GPIOA + 0x04), o8 = R32(GPIOA + 0x08), o2c = R32(GPIOA + 0x2C);
    uprintf("#X regs A: otype=%x r08=%x ds=%x  B: otype=%x r08=%x ds=%x\r\n", o4, o8, o2c,
            R32(GPIOB + 0x04), R32(GPIOB + 0x08), R32(GPIOB + 0x2C));
    uprintf("#X base      P1=%u REF=%u BAT=%u\r\n", adc_raw(1, 32) / 32, adc_raw(0, 32) / 32, adc_raw(8, 32) / 32);
    R32(GPIOA + 0x04) = 0;
    MilliSleep(2);
    uprintf("#X otype=0   P1=%u\r\n", adc_raw(1, 32) / 32);
    R32(GPIOA + 0x08) = 0x0A00;
    MilliSleep(2);
    uprintf("#X r08=a00   P1=%u\r\n", adc_raw(1, 32) / 32);
    R32(GPIOA + 0x2C) = 0xF4FF;
    MilliSleep(2);
    uprintf("#X ds=f4ff   P1=%u\r\n", adc_raw(1, 32) / 32);
    R32(GPIOA + 0x04) = o4;
    R32(GPIOA + 0x08) = o8;
    R32(GPIOA + 0x2C) = o2c;
    ADC_DDR_set(0);
    ADC_PORT_set(0);
    ADC_DDR_set(1 << 4);
    uprintf("#X end\r\n");
}

static void show_result(int found, uint32_t ms)
{
    char v[16], line[24];
    lcd_fill(0, 45, LCD_W, LCD_H - 45, C_BLACK);
    uprintf("#R found=%d vcc=%u fs=%u ref=%d ms=%u\r\n", found, measure_vcc(), measure_adc_fs(), measure_ref_ok(), ms);
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
        const struct mres *r = measure_result();
        lcd_text(4, 48, r->title, &font16, found >= 20 ? C_LABEL : C_WHITE, C_BLACK);
        uprintf("#R %s\r\n", r->title);
        for (int i = 0; i < r->n; i++) {
            if (i < 4)                                  /* rows 64..112: the 4th replaces the Vcc line */
                lcd_text(4, 64 + 16 * i, r->line[i], &font16, i ? C_WHITE : C_ACCENT, C_BLACK);
            uprintf("#R   %s\r\n", r->line[i]);
        }
        if (r->n >= 4)
            return;
    }
    fmt(line, sizeof line, "Vcc %u.%03uV%s", measure_vcc() / 1000, measure_vcc() % 1000, measure_ref_ok() ? "" : "?");
    lcd_text(4, 112, line, &font16, C_DIM, C_BLACK);
}

int main(void)
{
    board_init();
    uart_init();
    uprintf("\r\nlcrp1-fw " VERSION "\r\n");
    lcd_init();
    int adc_ok = adc_init();
    uprintf("adc init %d\r\n", adc_ok);
    ct_hal_init();
    measure_init();
    header();
    beep(40);

    int relay_on = 0, page = 0, redraw = 1, low = 0, shown = 0, c_test = 0, cmd_len = 0;
    char cmd[12];
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

        /* serial commands: '#' + letter [+ number] + CR/LF; anything else is ignored */
        int c = uart_getc();
        if (c == '#')
            cmd_len = 0, cmd[cmd_len++] = '#';
        else if (c >= 0 && cmd_len > 0 && c != '\r' && c != '\n' && cmd_len < (int)sizeof cmd - 1)
            cmd[cmd_len++] = (char)c;
        else if ((c == '\r' || c == '\n') && cmd_len >= 2) {
            cmd[cmd_len] = 0;
            unsigned arg = 0;
            for (int i = 2; i < cmd_len && cmd[i] >= '0' && cmd[i] <= '9'; i++)
                arg = arg * 10 + (unsigned)(cmd[i] - '0');
            char k = cmd[1];
            cmd_len = 0;
            if (k == 'T')
                c_test = 1;                     /* run a test */
            else if (k == 'D')
                probe_diag();
            else if (k == 'G')
                gate_trace();
            else if (k == 'X')
                drive_experiment();
            else if (k == 'W') {                /* sweep: measure with sample time codes 12..15 */
                uint8_t keep = adc_sample_code;
                for (uint8_t st = 12; st <= 15; st++) {
                    adc_sample_code = st;
                    uprintf("#W st=%u\r\n", st);
                    int f = measure_run();
                    uprintf("#W st=%u found=%d vcc=%u\r\n", st, f, measure_vcc());
                    for (int i = 0; i < measure_resistors(); i++) {
                        uint8_t ra, rb;
                        uint32_t rv;
                        int8_t rs;
                        measure_resistor(i, &ra, &rb, &rv, &rs);
                        uprintf("#W   R %u-%u %u e%d\r\n", ra, rb, rv, rs);
                    }
                    wdt_feed();
                }
                adc_sample_code = keep;
                uprintf("#W end\r\n");
            } else if (k == 'P' && arg <= 15) {
                ct_poll_code = (uint8_t)arg, uprintf("#P %u\r\n", arg);
            } else if (k == 'S' && arg <= 15)
                adc_sample_code = (uint8_t)arg, uprintf("#S %u\r\n", arg);
            else if (k == '?')
                uprintf("#? t=%u test=%d zener=%d ir=%d relay=%d adc=%d st=%u\r\n", g_ms, t, z, m, relay_on,
                        adc_ok, adc_sample_code);
        } else if (c == '\r' || c == '\n')
            cmd_len = 0;
    }
}
