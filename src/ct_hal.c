/* SPDX-License-Identifier: EUPL-1.2
 * Hardware layer for the ComponentTester code: virtual AVR-style probe ports, ADC, delays.
 * See ct_hal.h for the bit assignment.
 */
#include "ct_hal.h"
#include "adc.h"
#include "board.h"
#include "hw.h"

static uint8_t r_ddr, r_port, a_ddr, a_port;

struct vpin { uint32_t port; uint8_t pin; };
static const struct vpin r_pins[6] = {
    {GPIOA, 4}, {GPIOA, 5}, {GPIOA, 7}, {GPIOB, 1}, {GPIOB, 2}, {GPIOB, 10},
};
static const struct vpin a_pins[3] = {{GPIOA, 1}, {GPIOA, 3}, {GPIOA, 6}};
/* upstream channel numbers TP1, TP2, TP3, TP_ZENER, TP_REF, TP_BAT -> N32 ADC channels */
static const uint8_t adc_ch[6] = {1, 3, 6, 2, 0, 8};

/* Apply DDR/PORT of one virtual port to its pins: level first, then mode (no glitches).
 * High impedance = analog mode (as the stock firmware: no digital input buffer on probe pins). */
static void apply(const struct vpin *p, unsigned n, uint8_t ddr, uint8_t port)
{
    for (unsigned i = 0; i < n; i++) {
        uint32_t g = p[i].port;
        unsigned b = p[i].pin;
        if (ddr >> i & 1) {
            GP_BSC(g) = port >> i & 1 ? 1u << b : 1u << (b + 16);
            GP_MODE(g) = (GP_MODE(g) & ~(3u << 2 * b)) | (uint32_t)MODE_OUT << 2 * b;
        } else {
            GP_MODE(g) |= 3u << 2 * b;                  /* analog = high impedance */
        }
    }
}

void R_DDR_set(uint8_t v)   { r_ddr = v;  apply(r_pins, 6, r_ddr, r_port); }
void R_PORT_set(uint8_t v)  { r_port = v; apply(r_pins, 6, r_ddr, r_port); }
uint8_t R_DDR_get(void)     { return r_ddr; }
uint8_t R_PORT_get(void)    { return r_port; }
uint8_t ADC_DDR_get(void)   { return a_ddr; }
uint8_t ADC_PORT_get(void)  { return a_port; }

static void relay_update(void)
{
    relay(!(a_ddr >> 4 & 1));                           /* TP_REF driven low = probes shorted */
}

void ADC_DDR_set(uint8_t v)  { a_ddr = v;  apply(a_pins, 3, a_ddr, a_port); relay_update(); }
void ADC_PORT_set(uint8_t v) { a_port = v; apply(a_pins, 3, a_ddr, a_port); }

unsigned ct_adc_channel(uint8_t channel)
{
    return channel < 6 ? adc_ch[channel] : 17;
}

uint32_t ct_adc_read(uint8_t channel, uint8_t samples)
{
    return adc_raw(ct_adc_channel(channel), samples);
}

/* Fast level poll of a low-impedance node (e.g. a drain on Rl) for busy loops: short sample time */
uint8_t ct_poll_code = 4;
uint16_t ct_adc_poll(uint8_t channel)
{
    return (uint16_t)adc_raw_st(ct_adc_channel(channel), 1, ct_poll_code);
}

uint16_t ct_us(void)
{
    return (uint16_t)R32(0x40000800u + 0x24);           /* TIM3 counter, 1 MHz */
}

/* Probe pins have no digital input in analog mode: read the level with the ADC instead. */
uint8_t ADC_PIN_get(void)
{
    uint8_t v = 0;
    for (unsigned i = 0; i < 3; i++)
        if (adc_raw(adc_ch[i], 1) >= 2048)
            v |= 1u << i;
    return v;
}

/* TIM3: free-running 16-bit counter at 1 MHz */
#define TIM3       0x40000800u
#define T3_CR1     R32(TIM3 + 0x00)
#define T3_EGR     R32(TIM3 + 0x14)
#define T3_CNT     R32(TIM3 + 0x24)
#define T3_PSC     R32(TIM3 + 0x28)
#define T3_ARR     R32(TIM3 + 0x2C)
#define APB1_TIM3  0x0004u

void ct_wait_us(uint16_t us)
{
    uint16_t t0 = (uint16_t)T3_CNT;
    while ((uint16_t)((uint16_t)T3_CNT - t0) < us)
        ;
}

void MilliSleep(uint16_t ms)
{
    delay_ms(ms);                                       /* feeds the watchdog */
}

/* upstream's fixed delays (wait.S on the AVR) */
#define WAIT_US(n) void wait##n##us(void) { ct_wait_us(n); }
#define WAIT_MS(n) void wait##n##ms(void) { MilliSleep(n); }
WAIT_US(1) WAIT_US(2) WAIT_US(3) WAIT_US(4) WAIT_US(5) WAIT_US(10) WAIT_US(20) WAIT_US(30) WAIT_US(40)
WAIT_US(50) WAIT_US(100) WAIT_US(200) WAIT_US(300) WAIT_US(400) WAIT_US(500)
WAIT_MS(1) WAIT_MS(2) WAIT_MS(3) WAIT_MS(4) WAIT_MS(5) WAIT_MS(10) WAIT_MS(20) WAIT_MS(30) WAIT_MS(40)
WAIT_MS(50) WAIT_MS(100) WAIT_MS(200) WAIT_MS(300) WAIT_MS(400) WAIT_MS(500) WAIT_MS(1000)

void ct_hal_init(void)
{
    RCC_APB1EN |= APB1_TIM3;
    T3_CR1 = 0;
    T3_PSC = 47;                                        /* 48 MHz / 48 = 1 MHz */
    T3_ARR = 0xFFFF;
    T3_EGR = 1;
    T3_CR1 = 1;
    r_ddr = r_port = a_port = 0;
    a_ddr = 1u << 4;                                    /* safe state: probes shorted by the relay */
    apply(r_pins, 6, 0, 0);
    apply(a_pins, 3, 0, 0);
    relay_update();
}
