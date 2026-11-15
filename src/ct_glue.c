/* SPDX-License-Identifier: EUPL-1.2
 * Instantiates the ComponentTester globals and provides the pieces of the upstream ADC.c for the LCR-P1.
 */
#define MAIN_C
#include "config.h"
#include "common.h"
#include "variables.h"
#include "functions.h"

/* ADC full scale in mV (from the 2.5 V reference). On the AVR this equals Cfg.Vcc; on the LCR-P1 the
   probe pins drive only to about 4.4 V while the ADC full scale is about 5.0 V, so the two are kept
   apart: ReadU() converts with ct_adc_fs_mv, Cfg.Vcc is the measured drive level (src/measure.c). */
uint16_t ct_adc_fs_mv = UREF_VCC;

/* ReadU(): upstream ADC.c re-targeted: 12-bit ADC, no bandgap switching */
uint16_t ReadU(uint8_t Channel)
{
    uint32_t n = Cfg.Samples ? Cfg.Samples : 1;
    uint32_t sum = ct_adc_read(Channel & ADC_CHAN_MASK, (uint8_t)n);
    return (uint16_t)(sum * ct_adc_fs_mv / (4096u * n));
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
