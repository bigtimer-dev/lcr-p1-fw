/* SPDX-License-Identifier: EUPL-1.2
 * LCR-P1 open firmware: ComponentTester probing, results as text or as a symbol with probe numbers.
 * Page 0: TEST runs a probing cycle and shows the result. Page 1 (serial #A switches pages): ADC readings.
 * ZENER: Zener mode (K-A-A socket), TEST measures with the HV boost on.
 * Mode switch on IR: decodes remote controls (ir.c).
 * Hold TEST 1 s = off, idle = off after 1/3/10/30 min (hold ZENER 2 s), low battery = off. The HV boost is on only while a Zener reads.
 */
#include "adc.h"
#include "hw.h"
#include "ct_hal.h"
#include "measure.h"
#include "adjust.h"
#include "ir.h"
#include "sym.h"
#include "board.h"
#include "lcd.h"
#include "uart.h"

#define VERSION   "1.2"
#define IDLE_OFF_MS (nv_off_min() * 60000u)   /* 1, 3 (default), 10 or 30 min: hold ZENER 2 s */
#define LOWBAT_MV 3300          /* as stock: switch off at or below this */

static void row(int y, const char *label, const char *value, uint16_t c)
{
    int x = lcd_text(4, y, label, &font16, C_LABEL, C_BLACK);
    x = lcd_text(x < 44 ? 44 : x, y, value, &font16, c, C_BLACK);
    lcd_fill(x, y, LCD_W - x, 16, C_BLACK);
}

/* battery: LiPo charge from the voltage (resting curve; reads higher while charging) */
static const uint16_t bat_curve[][2] = {
    {3300, 0}, {3500, 5}, {3600, 10}, {3700, 25}, {3750, 40}, {3800, 50}, {3850, 60}, {3900, 68},
    {4000, 80}, {4100, 92}, {4200, 100},
};
static int bat_pct = -1;                        /* shown value, -1 = none yet */

static int bat_percent(uint32_t mv)
{
    if (mv <= bat_curve[0][0])
        return 0;
    for (unsigned i = 1; i < sizeof bat_curve / sizeof bat_curve[0]; i++)
        if (mv < bat_curve[i][0])
            return bat_curve[i - 1][1] + (int)(mv - bat_curve[i - 1][0]) * (bat_curve[i][1] - bat_curve[i - 1][1])
                                         / (bat_curve[i][0] - bat_curve[i - 1][0]);
    return 100;
}

/* icon at the top right of the header: outline and level bar, red below 15 %. (No charging sign: the board
   has no USB/charge status line; the CH340 runs from the 3.3 V rail, so its RX line stays high.) */
static void bat_icon(void)
{
    const int x = 106, y = 6, w = 18, h = 10;
    if (bat_pct < 0)
        return;
    uint16_t c = bat_pct < 15 ? C_RED : C_LABEL;
    lcd_fill(x, y, w, 1, c);
    lcd_fill(x, y + h - 1, w, 1, c);
    lcd_fill(x, y, 1, h, c);
    lcd_fill(x + w - 1, y, 1, h, c);
    lcd_fill(x + w, y + 3, 2, h - 6, c);
    int f = (w - 4) * bat_pct / 100;
    if (f < 1)
        f = 1;
    lcd_fill(x + 2, y + 2, f, h - 4, c);
    lcd_fill(x + 2 + f, y + 2, w - 4 - f, h - 4, C_BLACK);
}

static void header(void)
{
    lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    lcd_text((LCD_W - lcd_text_width("LCR-P1", &font24)) / 2, 2, "LCR-P1", &font24, C_ACCENT, C_BLACK);
    lcd_text((LCD_W - lcd_text_width("open fw " VERSION, &font16)) / 2, 26, "open fw " VERSION, &font16,
             C_LABEL, C_BLACK);
    lcd_fill(4, 44, LCD_W - 8, 1, C_DIM);
    bat_icon();
}

static const struct { const char *name; uint8_t ch; uint8_t mul; } adc_rows[] = {
    {"BAT", ADC_CH_BAT, 2}, {"REF", ADC_CH_REF, 1}, {"HV", ADC_CH_ZENER, 10}, {"P1", ADC_CH_P1, 1},
    {"P2", ADC_CH_P2, 1}, {"P3", ADC_CH_P3, 1}, {"16", 16, 1}, {"17", 17, 1},
};
#define NROWS (sizeof adc_rows / sizeof adc_rows[0])

static void off(const char *why)
{
    gpio_set(PIN_HV, 0);
    relay(0);
    uprintf("power off (%s)\r\n", why);
    lcd_fill(0, 45, LCD_W, LCD_H - 45, C_BLACK);
    lcd_text(28, 64, "power off", &font16, C_ACCENT, C_BLACK);
    lcd_text((LCD_W - lcd_text_width(why, &font16)) / 2, 84, why, &font16, C_LABEL, C_BLACK);
    beep(80);
    delay_ms(600);
    power_off();
}

/* serial #U: hand over to the LCR-P1 bootloader's update mode (boot/bl.c: request words at 0x20001FF8 and
   a jump to its reset vector). FNIRSI's loader has no such entry: then nothing happens. */
static void enter_bootloader(void)
{
    const volatile uint32_t *info = (const volatile uint32_t *)0x080017F0u;
    if (info[0] != 0x314C424Cu || info[3] != ~0x314C424Cu) {
        uprintf("#U no LCR-P1 bootloader (FNIRSI loader: hold ZENER, press TEST)\r\n");
        return;
    }
    uprintf("#U bootloader %u.%u\r\n", (unsigned)(info[1] >> 8 & 255), (unsigned)(info[1] & 255));
    gpio_set(PIN_HV, 0);
    relay(0);
    delay_ms(5);                                /* reply out */
    __asm volatile("cpsid i");
    T6_DIER = 0;
    T6_CR1 = 0;
    T1_CCER = 0;                                /* buzzer, backlight PWM off */
    *(volatile uint32_t *)0x20001FF8u = 0xB007104Du;
    *(volatile uint32_t *)0x20001FFCu = ~0xB007104Du;
    uint32_t sp = R32(0x08000000u), pc = R32(0x08000004u);
    __asm volatile("msr msp, %0\n bx %1" : : "r"(sp), "r"(pc));
}

static int no_header;                          /* a symbol result used the whole screen */

/* symbol results: title at the top, the symbol with probe numbers at its terminals, values below */
static void sym_screen(const char *title, uint8_t sym, const uint8_t *pins, int npins)
{
    char g[2] = {(char)sym, 0};
    lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    no_header = 1;
    lcd_text(2, 1, title, &font16, C_LABEL, C_BLACK);
    int x0 = (LCD_W - SYM_W) / 2, y0 = 19;
    lcd_text(x0, y0, g, &sym_font, C_WHITE, C_BLACK);
    for (int i = 0; i < npins; i++) {
        char d[2] = {(char)(pins[i] < 10 ? '0' + pins[i] : pins[i]), 0};
        lcd_text(x0 + sym_label[sym - 1][i][0], y0 + sym_label[sym - 1][i][1], d, &font16, C_ACCENT, C_BLACK);
    }
}

/* "label value" row: label in font16, value in font24 */
static void big_row(int y, const char *label, const char *value)
{
    lcd_text(4, y + 6, label, &font16, C_LABEL, C_BLACK);
    lcd_text(4 + 8 * 3, y, value, &font24, C_ACCENT, C_BLACK);
}

/* Zener mode (ZENER key): screen with the K-A-A socket; TEST measures once. The boost converter (PF6)
   feeds the socket through a ~5 mA current limit only during the measurement (relay on, as in the stock
   firmware; the socket voltage rises over ~1 s as the output charges); PA2 reads it through a 10:1 divider. An empty socket reads the boost's open voltage (37 V on
   the meter): "open" above 33 V. ZENER or the IR switch leave. Returns the time of the last key press. */
#define ZENER_OPEN_MV 33000                     /* above: socket open (stock limit 33 V) */
static uint32_t zener_read(void)
{
    relay(1);
    gpio_set(PIN_HV, 1);
    uint32_t t0 = g_ms, mv = 0, prev = ~0u;
    while (g_ms - t0 < 3000) {                  /* settled: two readings 100 ms apart within 0.1 % */
        delay_ms(100);
        wdt_feed();
        uint32_t raw = adc_raw(ADC_CH_ZENER, 16) / 16;
        mv = raw * 10 * adc_vref_mv / 4096;
        uprintf("#Z %u %u\r\n", mv, raw);
        if (g_ms - t0 >= 500 && prev != ~0u && (mv > prev ? mv - prev : prev - mv) * 1000 <= prev + 10)
            break;
        prev = mv;
    }
    gpio_set(PIN_HV, 0);
    relay(0);
    return mv;
}

static void zener_screen(void)
{
    static const uint8_t pins[2] = {'K', 'A'};
    sym_screen("Zener", SYM_ZENER, pins, 2);
    lcd_text(4, 112, "TEST: measure", &font16, C_LABEL, C_BLACK);
}

static uint32_t zener_mode(void)
{
    char b[16];
    uint32_t idle = g_ms;
    zener_screen();
    uprintf("zener mode\r\n");
    for (;;) {
        wdt_feed();
        beep_poll();
        int ev = key_poll();
        if (ev == KEY_TEST_LONG)
            off("key");
        if (ev != KEY_NONE)
            idle = g_ms;
        if (ev == KEY_ZENER || mode_ir())
            break;
        if (g_ms - idle > IDLE_OFF_MS)
            off("idle");
        if (ev != KEY_TEST)
            continue;
        lcd_fill(0, 66, LCD_W, 40, C_BLACK);
        lcd_text(4, 74, "measuring...", &font16, C_LABEL, C_BLACK);
        uint32_t mv = zener_read();
        lcd_fill(0, 66, LCD_W, 40, C_BLACK);
        if (mv > ZENER_OPEN_MV || mv < 100) {
            const char *t = mv < 100 ? "short" : "open";
            lcd_text((LCD_W - lcd_text_width(t, &font24)) / 2, 72, t, &font24, C_WHITE, C_BLACK);
            uprintf("#R Zener %s\r\n", t);
            beep(20);
        } else {
            fmt(b, sizeof b, "%u.%02uV", mv / 1000, mv % 1000 / 10);
            big_row(72, "Vz", b);
            uprintf("#R Zener %u mV\r\n", mv);
            beep(60);
        }
        idle = g_ms;
    }
    uprintf("zener off\r\n");
    return idle;
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

static void adj_say(const char *l1, const char *l2)
{
    lcd_fill(0, 45, LCD_W, LCD_H - 45, C_BLACK);
    lcd_text(4, 56, l1, &font16, C_WHITE, C_BLACK);
    lcd_text(4, 76, l2, &font16, C_LABEL, C_BLACK);
    uprintf("#J %s %s\r\n", l1, l2);
}

static int adj_key(void)                            /* TEST continues, ZENER cancels; serial T / Z alike */
{
    wdt_feed();
    int c = uart_getc();
    if (c == 'T' || c == 'Z')
        return c == 'T' ? 1 : 2;
    int ev = key_poll();
    return ev == KEY_TEST || ev == KEY_TEST_LONG ? 1 : ev == KEY_ZENER ? 2 : 0;
}

static const struct adjust_ui adj_ui = {adj_say, adj_key};

/* settings (hold ZENER 2 s): auto power-off time. ZENER changes, TEST saves, 10 s untouched = cancel */
static void settings(void)
{
    static const uint8_t opts[] = {1, 3, 10, 30};
    unsigned k = 0, cur = nv_off_min();
    while (opts[k] != cur)
        k++;
    char b[16];
    uprintf("settings\r\n");
    header();
    lcd_text(4, 48, "Auto power-off", &font16, C_LABEL, C_BLACK);
    lcd_text(4, 96, "ZENER: change", &font16, C_LABEL, C_BLACK);
    lcd_text(4, 112, "TEST: save", &font16, C_LABEL, C_BLACK);
    uint32_t t0 = g_ms;
    for (int draw = 1;; ) {
        wdt_feed();
        beep_poll();
        if (draw) {
            fmt(b, sizeof b, "%u min", opts[k]);
            lcd_fill(0, 66, LCD_W, 26, C_BLACK);
            lcd_text((LCD_W - lcd_text_width(b, &font24)) / 2, 68, b, &font24, C_ACCENT, C_BLACK);
            draw = 0;
        }
        int ev = key_poll();
        if (ev == KEY_ZENER)
            k = (k + 1) % sizeof opts, draw = 1, t0 = g_ms;
        else if (ev == KEY_TEST) {
            int ok = nv_set_off_min(opts[k]);
            uprintf("auto-off %u min %s\r\n", opts[k], ok ? "saved" : "SAVE FAILED");
            beep(40);
            break;
        } else if (ev == KEY_TEST_LONG)
            off("key");
        if (g_ms - t0 > 10000) {
            uprintf("settings: no change\r\n");
            break;
        }
    }
}

static void show_result(int found, uint32_t ms)
{
    char v[16], line[24];
    lcd_fill(0, 45, LCD_W, LCD_H - 45, C_BLACK);
    uprintf("#R found=%d vcc=%u fs=%u ref=%d ms=%u\r\n", found, measure_vcc(), measure_adc_fs(), measure_ref_ok(), ms);
    int n = measure_resistors();
    if (n == 1) {                                       /* resistor or inductor: symbol layout */
        uint8_t a, b, pins[2];
        uint32_t value, lv;
        int8_t scale, ls;
        measure_resistor(0, &a, &b, &value, &scale);
        pins[0] = a, pins[1] = b;
        measure_format(v, sizeof v, value, scale, CH_OHM);
        uprintf("#R R %u-%u %u e%d\r\n", a, b, value, scale);
        if (measure_inductor(&lv, &ls)) {
            sym_screen("Inductor", SYM_IND, pins, 2);
            char l[16];
            measure_format(l, sizeof l, lv, ls, "H");
            big_row(68, "L", l);
            big_row(98, "R", v);
            uprintf("#R L %u e%d\r\n", lv, ls);
        } else {
            sym_screen("Resistor", SYM_RES, pins, 2);
            big_row(76, "R", v);
        }
        return;
    }
    const struct mres *r = measure_result();
    if (!n && r->sym) {
        uprintf("#R %s\r\n", r->title);
        for (int i = 0; i < r->n; i++)
            uprintf("#R   %s\r\n", r->line[i]);
        int two = r->sym <= SYM_DIODE;
        sym_screen(r->title, r->sym, r->pins, two ? 2 : 3);
        int y = 66, i = r->first;
        if (two && i < r->n) {                          /* main value large: "C 4.7uF", "A1 K2 0.65V" */
            const char *t = r->line[i], *val = t;
            for (const char *p = t; *p; p++)
                if (*p == ' ')
                    val = p + 1;
            char lab[4] = {0};
            if (r->sym == SYM_DIODE)
                lab[0] = 'V', lab[1] = 'f';
            else
                for (int k = 0; k < 3 && t[k] && t[k] != ' '; k++)
                    lab[k] = t[k];
            big_row(y, lab, val);
            y += 30, i++;
        }
        for (; i < r->n && y <= LCD_H - 16; i++, y += 16)
            lcd_text(4, y, r->line[i], &font16, C_WHITE, C_BLACK);
        return;
    }
    if (n) {
        lcd_text(4, 48, measure_result()->title, &font16, C_LABEL, C_BLACK);
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
        uint32_t lv;
        int8_t ls;
        if (measure_inductor(&lv, &ls)) {                   /* single resistor: room for the L row */
            measure_format(v, sizeof v, lv, ls, "H");
            lcd_text(4, 98, "L", &font16, C_LABEL, C_BLACK);
            lcd_text(36, 94, v, &font24, C_ACCENT, C_BLACK);
            uprintf("#R L %u e%d\r\n", lv, ls);
            return;                                         /* the L row takes the Vcc line's place */
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

/* IR mode (slide switch): the screen shows the last decoded remote code */
static void ir_screen(void)
{
    header();
    lcd_text(4, 48, "IR remote", &font16, C_LABEL, C_BLACK);
    lcd_text(4, 72, "waiting...", &font16, C_DIM, C_BLACK);
}

static void ir_receive(void)
{
    const uint16_t *b;
    struct ir_result r;
    char v[20];
    int n = ir_capture(&b);
    int ok = ir_decode(b, n, &r);
    if (ok && !r.nbytes)                        /* NEC repeat: keep the code on screen */
        return;
    lcd_fill(0, 45, LCD_W, LCD_H - 45, C_BLACK);
    if (!ok) {
        lcd_text(4, 48, "IR: unknown", &font16, C_LABEL, C_BLACK);
        fmt(v, sizeof v, "%d edges", n - 1);
        lcd_text(4, 72, v, &font16, C_WHITE, C_BLACK);
        lcd_text(4, 96, "timings: serial", &font16, C_DIM, C_BLACK);
    } else if (r.nbytes == 8) {                 /* AC: whole state, two rows of 4 bytes */
        lcd_text(4, 48, r.name, &font16, C_LABEL, C_BLACK);
        for (int row = 0; row < 2; row++) {
            const uint8_t *c = r.code + 4 * row;
            fmt(v, sizeof v, "%02X %02X %02X %02X", c[0], c[1], c[2], c[3]);
            lcd_text(4, 68 + 26 * row, v, &font24, C_ACCENT, C_BLACK);
        }
    } else {
        lcd_text(4, 48, r.name, &font16, C_LABEL, C_BLACK);
        fmt(v, sizeof v, "%02X%02X", r.code[0], r.code[1]);
        lcd_text(4, 72, "Addr", &font16, C_LABEL, C_BLACK);
        lcd_text(52, 68, v, &font24, C_ACCENT, C_BLACK);
        fmt(v, sizeof v, "%02X%02X", r.code[2], r.code[3]);
        lcd_text(4, 98, "Cmd", &font16, C_LABEL, C_BLACK);
        lcd_text(52, 94, v, &font24, C_ACCENT, C_BLACK);
    }
    if (ok) {
        uprintf("#I %s", r.name);
        for (int k = 0; k < r.nbytes; k++)
            uprintf(" %02X", r.code[k]);
        uprintf("\r\n");
    }
    if (!ok || n > 100) {                       /* unknown or long (AC): the timings, mark space ... in us */
        uprintf("#I raw %d:", n - 1);
        for (int i = 1; i < n; i++)
            uprintf(" %u", b[i]);
        uprintf("\r\n");
    }
    beep(20);
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
        if (mode_ir()) {                        /* IR mode: only the receiver (and power-off) */
            if (last_m != 1) {
                uprintf("mode IR\r\n");
                ir_screen();
                last_m = 1;
            }
            if (ir_active()) {
                ir_receive();
                idle = g_ms;
            }
            if (ev == KEY_TEST_LONG)
                off("key");
            if (g_ms - idle > IDLE_OFF_MS)
                off("idle");
            continue;
        }
        if (last_m == 1) {                      /* back from IR mode */
            page = 0, redraw = 1, shown = 0;
            last_m = -1;                        /* logs "mode normal" below */
        }
        if ((ev == KEY_TEST && key_zener_down()) || c_test == 2) {   /* ZENER held + TEST: self-adjust */
            c_test = 0;
            page = 0, header();
            int n = adjust_run(&adj_ui);
            char b[17];
            fmt(b, sizeof b, n > 0 ? "%d values saved" : "nothing changed", n);
            adj_say(n > 0 ? "Adjust done" : "Adjust failed", b);
            redraw = 0, last_t = last_z = last_r = last_m = -2;
            shown = 1;
            idle = g_ms;                        /* no beeps in the self-adjust (user's choice) */
        } else if (ev == KEY_TEST || c_test) {
            c_test = 0;                         /* no beep here: the test blocks the loop, it would last */
            if (page)
                page = 0, redraw = 1, header();
            lcd_fill(0, 45, LCD_W, LCD_H - 45, C_BLACK);
            if (no_header)
                header(), no_header = 0;
            lcd_text(4, 56, "probing...", &font16, C_LABEL, C_BLACK);
            uint32_t t0 = g_ms;
            int found = measure_run();
            show_result(found, g_ms - t0);
            nv_autosave();                      /* CompOffset follows the cap measurements */
            redraw = 0, last_t = last_z = last_r = last_m = -2;     /* keep the result on screen */
            shown = 1;
            beep(found ? 60 : 20);
        } else if (ev == KEY_ZENER_LONG) {
            settings();
            idle = g_ms;
            page = 0, redraw = 1, shown = 0;
            continue;
        } else if (ev == KEY_ZENER) {
            beep(20);
            idle = zener_mode();
            page = 0, redraw = 1, shown = 0;
            continue;
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
            static uint8_t bat_skip = 1;        /* first reading after power-on is low (divider settling) */
            int pct = bat_percent(bat);
            if (bat_skip)
                bat_skip = 0;
            else if (bat_pct < 0 || pct > bat_pct + 1 || pct < bat_pct - 1 || (pct == 0) != (bat_pct == 0)
                || (pct < 15) != (bat_pct < 15)) {                  /* hysteresis: no 89/90 % flicker */
                bat_pct = pct;
                uprintf("#B %d%%\r\n", pct);
                if (page == 0 && !no_header)
                    bat_icon();
            }
            if (page == 0 && !shown && bat_pct >= 0) {
                fmt(buf, sizeof buf, "%u.%02uV %d%%", bat / 1000, bat % 1000 / 10, bat_pct);
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
            idle = g_ms;                        /* a serial command counts as use (no idle power-off) */
            unsigned arg = 0;
            for (int i = 2; i < cmd_len && cmd[i] >= '0' && cmd[i] <= '9'; i++)
                arg = arg * 10 + (unsigned)(cmd[i] - '0');
            char k = cmd[1];
            cmd_len = 0;
            if (k == 'T')
                c_test = 1;                     /* run a test */
            else if (k == 'J')
                c_test = 2;                     /* self-adjustment */
            else if (k == 'N')
                nv_print("current");
            else if (k == 'U')
                enter_bootloader();             /* returns only without an LCR-P1 bootloader */
            else if (k == 'A')
                page = !page, redraw = 1, shown = 0, uprintf("#A page %d\r\n", page);
#ifdef CT_DEBUG
            else if (k == 'E' && arg <= 15) {   /* ADC sampling instant scan (sets the ESR timing) */
                relay(1);                       /* probes free */
                delay_ms(30);
                adc_timing_scan(arg);
                relay(0);
                uprintf("#E end\r\n");
            }
#endif
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
