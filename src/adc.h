/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef ADC_H
#define ADC_H
#include <stdint.h>

/* channels */
#define ADC_CH_REF    0     /* PA0: reference input (stock: x2 = VDD estimate) */
#define ADC_CH_P1     1     /* PA1: probe 1 */
#define ADC_CH_ZENER  2     /* PA2: Zener HV divider (x10) */
#define ADC_CH_P2     3     /* PA3: probe 2 */
#define ADC_CH_P3     6     /* PA6: probe 3 */
#define ADC_CH_BAT    8     /* PB0: battery divider (x2) */

extern uint16_t adc_vref_mv;            /* ADC full scale in mV (stock default 4940) */

int adc_init(void);                     /* 0 = ready */
uint32_t adc_raw(unsigned ch, unsigned n);      /* sum of n conversions (12 bit each) */
uint32_t adc_mv(unsigned ch, unsigned n);       /* average in mV */
uint32_t battery_mv(void);

#endif
