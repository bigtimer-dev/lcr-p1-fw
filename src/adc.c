/* SPDX-License-Identifier: EUPL-1.2
 * ADC: configuration and conversion steps as the stock firmware does them.
 */
#include "adc.h"
#include "board.h"
#include "hw.h"
#include "uart.h"

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

void adc_select(unsigned ch, unsigned st)
{
    volatile uint32_t *r = ch > 15 ? &ADC_SAMPT1 : ch > 7 ? &ADC_SAMPT2 : &ADC_SAMPT3;
    unsigned s = 4 * (ch & 7);
    *r = (*r & ~(15u << s)) | st << s;
    ADC_RSEQ3 = (ADC_RSEQ3 & ~31u) | ch;
}

static uint32_t convert(void)
{
    ADC_CR2 |= 0x500000;                        /* start */
    uart_poll();                                /* conversions block the main loop for ms in a row */
    for (unsigned k = 0; !(ADC_STS & 2); k++)
        if (k > 100000)
            return 0;
    ADC_STS &= ~2u;
    return ADC_DAT & 0xFFF;
}

uint32_t adc_raw_st(unsigned ch, unsigned n, unsigned st)
{
    adc_select(ch, st);
    (void)convert();                            /* the data register lags one conversion behind (seen on
                                                   the meter): the first result of every read is stale */
    uint32_t sum = 0;
    while (n--)
        sum += convert();
    return sum;
}

uint32_t adc_raw(unsigned ch, unsigned n)
{
    return adc_raw_st(ch, n, adc_sample_code);
}

uint32_t adc_mv(unsigned ch, unsigned n)
{
    return adc_raw(ch, n) * adc_vref_mv / (4096u * n);
}

uint32_t battery_mv(void)
{
    return 2 * adc_mv(ADC_CH_BAT, 8);
}

/* ESR groundwork: when does a conversion sample its input after the start command? Probe 1 (PA1,
   relay open, empty socket) is driven low, a conversion started, PA1 raised after k delay-loop runs;
   prints the data register 30 us later and the result of the next conversion (pin low again). */
static void loop_delay(uint32_t n)
{
    __asm volatile(".syntax unified\n1: subs %0, %0, #1\n bne 1b" : "+l"(n) : : "cc");
}

void adc_timing_scan(unsigned st)
{
    uint32_t mode = GP_MODE(GPIOA);
    adc_select(ADC_CH_P1, st);
    (void)convert();
    for (unsigned k = 0; k <= 120; k += (k < 60 ? 1 : 4)) {
        GP_BSC(GPIOA) = 1u << (1 + 16);
        GP_MODE(GPIOA) = (mode & ~(3u << 2)) | (uint32_t)MODE_OUT << 2;
        loop_delay(400);
        ADC_STS &= ~2u;
        ADC_CR2 |= 0x500000;
        if (k)
            loop_delay(k);
        GP_BSC(GPIOA) = 1u << 1;
        loop_delay(500);
        uint32_t sts = ADC_STS, dat = ADC_DAT & 0xFFF;
        GP_BSC(GPIOA) = 1u << (1 + 16);
        loop_delay(400);
        uint32_t next = convert();
        uprintf("#E st=%u k=%u dat=%u next=%u sts=%x\r\n", st, k, dat, next, sts);
        wdt_feed();
    }
    GP_MODE(GPIOA) = mode;
}
