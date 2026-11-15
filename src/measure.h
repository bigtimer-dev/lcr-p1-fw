/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef MEASURE_H
#define MEASURE_H
#include <stdint.h>

void measure_init(void);            /* after board/ADC init: load adjustment defaults, set Cfg */
int measure_prepare(void);          /* refs, discharge, drive level (relay must be open); 0 = error */
int measure_run(void);              /* one probing cycle (ComponentTester sequence); returns Check.Found */
uint16_t measure_vcc(void);         /* probe drive level (mV) measured in the last cycle */
uint16_t measure_adc_fs(void);      /* ADC full scale (mV) from the 2.5 V reference */
int measure_ref_ok(void);           /* 2.5 V reference was in range */
/* result access for the UI */
int measure_resistors(void);
void measure_resistor(int i, uint8_t *a, uint8_t *b, uint32_t *value, int8_t *scale);
int measure_inductor(uint32_t *value, int8_t *scale);     /* 1: the single resistor has inductance */
/* text result for any component: title + up to MRES_LINES lines of at most 16 characters */
#define MRES_LINES 6
struct mres {
    char title[17];
    int n;
    char line[MRES_LINES][17];
    uint8_t sym;                    /* SYM_x (sym.h) for the result screen, 0 = text only */
    uint8_t pins[3];                /* probe number (1..3) per symbol terminal */
    uint8_t first;                  /* first line[] shown under the symbol (skips the pinout line) */
};
const struct mres *measure_result(void);   /* text of the last measure_run() */
/* "1.234k" style text, 4 significant digits */
void measure_format(char *out, int size, uint32_t value, int8_t scale, const char *unit);

#endif
