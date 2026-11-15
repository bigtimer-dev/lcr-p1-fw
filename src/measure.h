/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef MEASURE_H
#define MEASURE_H
#include <stdint.h>

void measure_init(void);            /* after board/ADC init: load adjustment defaults, set Cfg */
int measure_run(void);              /* one probing cycle (ComponentTester sequence); returns Check.Found */
uint16_t measure_vcc(void);         /* Vcc (mV) from the last cycle (2.5 V reference) */
int measure_ref_ok(void);           /* 2.5 V reference was in range */
/* result access for the UI */
int measure_resistors(void);
void measure_resistor(int i, uint8_t *a, uint8_t *b, uint32_t *value, int8_t *scale);
/* "1.234k" style text, 4 significant digits */
void measure_format(char *out, int size, uint32_t value, int8_t scale, const char *unit);

#endif
