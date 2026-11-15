/* SPDX-License-Identifier: EUPL-1.2
 * ADC: configuration and conversion steps as the stock firmware does them.
 */
#include "adc.h"
#include "board.h"
#include "hw.h"

uint16_t adc_vref_mv = 4940;
uint8_t adc_sample_code = 14;                   /* 4-bit sample time code. Stock uses 12/13; 12 does not settle
                                                   from 470k sources (1M rejected on the meter), 13 just does,
                                                   15 gave phantom results: 14 */

int adc_init(void)
{
    RCC_AHBEN |= AHB_ADC;
    RCC_CFGR2 = (RCC_CFGR2 & ~0x10Fu) | 5;      /* ADC clock as stock (CFGR2 = 0x3805) */
    ADC_CR1 = 0;
    ADC_CR2 = 0x000E0000;                       /* software trigger, single, right aligned */
    ADC_RSEQ1 = 0;                              /* one conversion */
    ADC_CR2 |= 1;                               /* on */
    for (uint32_t t = g_ms; !(ADC_CR3 & 0x20); )
        if (g_ms - t > 100)
            return -1;
    for (uint32_t t = g_ms; ADC_CR3 & 0x40; )
        if (g_ms - t > 100)
            return -2;
    return 0;
}

static void select(unsigned ch, unsigned st)
{
    volatile uint32_t *r = ch > 15 ? &ADC_SAMPT1 : ch > 7 ? &ADC_SAMPT2 : &ADC_SAMPT3;
    unsigned s = 4 * (ch & 7);
    *r = (*r & ~(15u << s)) | st << s;
    ADC_RSEQ3 = (ADC_RSEQ3 & ~31u) | ch;
}

static uint32_t convert(void)
{
    ADC_CR2 |= 0x500000;                        /* start */
    for (unsigned k = 0; !(ADC_STS & 2); k++)
        if (k > 100000)
            return 0;
    ADC_STS &= ~2u;
    return ADC_DAT & 0xFFF;
}

uint32_t adc_raw(unsigned ch, unsigned n)
{
    select(ch, adc_sample_code);
    (void)convert();                            /* the data register lags one conversion behind (seen on
                                                   the meter): the first result of every read is stale */
    uint32_t sum = 0;
    while (n--)
        sum += convert();
    return sum;
}

uint32_t adc_mv(unsigned ch, unsigned n)
{
    return adc_raw(ch, n) * adc_vref_mv / (4096u * n);
}

uint32_t battery_mv(void)
{
    return 2 * adc_mv(ADC_CH_BAT, 8);
}
