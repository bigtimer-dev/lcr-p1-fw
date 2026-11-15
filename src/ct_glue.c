/* SPDX-License-Identifier: EUPL-1.2
 * Instantiates the ComponentTester globals and provides the pieces of the upstream ADC.c for the LCR-P1.
 */
#define MAIN_C
#include "config.h"
#include "common.h"
#include "variables.h"
#include "functions.h"

/* ReadU(): upstream ADC.c re-targeted: 12-bit ADC, reference = VDD (Cfg.Vcc), no bandgap switching */
uint16_t ReadU(uint8_t Channel)
{
    uint32_t sum = ct_adc_read(Channel & ADC_CHAN_MASK, Cfg.Samples ? Cfg.Samples : 1);
    uint32_t n = Cfg.Samples ? Cfg.Samples : 1;
    return (uint16_t)(sum * Cfg.Vcc / (4096u * n));
}

uint16_t ReadU_5ms(uint8_t Channel)
{
    wait5ms();
    return ReadU(Channel);
}

uint16_t ReadU_20ms(uint8_t Channel)
{
    wait20ms();
    return ReadU(Channel);
}
