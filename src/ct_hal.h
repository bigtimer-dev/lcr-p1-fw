/* SPDX-License-Identifier: EUPL-1.2
 * Hardware layer for the ComponentTester code on the LCR-P1.
 *
 * Virtual 8-bit ports with ATmega semantics (DDR bit 1 = output with PORT level, 0 = high impedance):
 *   R port:   bit0 Rl1 PA4, bit1 Rh1 PA5, bit2 Rl2 PA7, bit3 Rh2 PB1, bit4 Rl3 PB2, bit5 Rh3 PB10
 *   ADC port: bit0 TP1 PA1, bit1 TP2 PA3, bit2 TP3 PA6, bit4 TP_REF -> discharge relay PB3
 *             (DDR bit4 = 1: relay off, probes shorted; 0: relay on, probes free; as HW_DISCHARGE_RELAY)
 * ADC channels in the upstream numbering (TP1..TP_BAT = 0..5) are mapped to the N32 channels.
 */
#ifndef CT_HAL_H
#define CT_HAL_H
#include <stdint.h>

void ct_hal_init(void);

void R_DDR_set(uint8_t v);
void R_PORT_set(uint8_t v);
void ADC_DDR_set(uint8_t v);
void ADC_PORT_set(uint8_t v);
uint8_t R_DDR_get(void);
uint8_t R_PORT_get(void);
uint8_t ADC_DDR_get(void);
uint8_t ADC_PORT_get(void);
uint8_t ADC_PIN_get(void);              /* probe levels (threshold VDD/2) */

uint32_t ct_adc_read(uint8_t channel, uint8_t samples);    /* upstream channel number; sum of 12-bit codes */
unsigned ct_adc_channel(uint8_t channel);
uint16_t ct_adc_poll(uint8_t channel);                     /* one fast 12-bit reading (sample code ct_poll_code) */
extern uint8_t ct_poll_code;
uint16_t ct_us(void);                                      /* free-running us counter */                  /* upstream -> N32 ADC channel */

/* delays (µs timer: TIM3 at 1 MHz) */
void ct_wait_us(uint16_t us);

/* Small-cap timing (cap.c SmallCap): COMP1 compares the sense pin of probe TPn (n = channel 0..2) with VREF1
 * = VDD * CT_COMP_VREF / 64; TIM3 CH1 captures its rising edge at 48 MHz. ct_cap_charge() releases the
 * probes not in ddr (ADC_DDR value while charging) and returns the ticks (bit 0..15) and timer overflows
 * (bit 16..31); overflows == max_ovf means timeout. */
#define CT_COMP_VREF 15
/* ESR: 255 x (U_1 + U_3) and (U_2 + U_4) raw ADC sums as upstream MeasureESR (probe IDs 0..2); 0 = ADC error */
uint8_t ct_esr_sums(uint8_t id1, uint8_t id2, uint32_t *sum1, uint32_t *sum2);
extern uint8_t ct_esr_ks, ct_esr_kp;
uint32_t ct_cap_charge(uint8_t channel, uint8_t ddr, uint16_t max_ovf);
void MilliSleep(uint16_t ms);
#ifdef CT_DEBUG
extern struct ct_prof { uint32_t adc_us, n_adc, sleep_ms, n_sleep; } ct_prof;
#endif
/* waitNNus() / waitNNms(): declared by upstream functions.h, implemented in ct_hal.c */
#endif
