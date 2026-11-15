/* SPDX-License-Identifier: EUPL-1.2
 * Hardware layer for the ComponentTester code: virtual AVR-style probe ports, ADC, delays.
 * See ct_hal.h for the bit assignment.
 */
#include "ct_hal.h"
#include "adc.h"
#include "board.h"
#include "hw.h"
#include "uart.h"

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

#ifdef CT_DEBUG
struct ct_prof ct_prof;                                 /* where the time of a test goes (#T lines) */
#endif

uint32_t ct_adc_read(uint8_t channel, uint8_t samples)
{
#ifdef CT_DEBUG
    uint16_t t0 = (uint16_t)R32(0x40000824u);
    uint32_t v = adc_raw(ct_adc_channel(channel), samples);
    ct_prof.adc_us += (uint16_t)((uint16_t)R32(0x40000824u) - t0);
    ct_prof.n_adc++;
    return v;
#else
    return adc_raw(ct_adc_channel(channel), samples);
#endif
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
        if (us >= 100)
            uart_poll();                                /* long waits: keep serial input (short ones are timing) */
}

/* On the 1 MHz timer: delay_ms() counts 1 kHz tick edges, so its first ms is anywhere from 0 to 1 ms long,
 * which made cap.c's 1 ms charging pulses (4.7-47 uF) up to 2x short and the capacitance that much too high. */
void MilliSleep(uint16_t ms)
{
#ifdef CT_DEBUG
    ct_prof.sleep_ms += ms;
    ct_prof.n_sleep++;
#endif
    while (ms--) {
        ct_wait_us(1000);
        wdt_feed();
    }
}

/* upstream's fixed delays (wait.S on the AVR) */
#define WAIT_US(n) void wait##n##us(void) { ct_wait_us(n); }
#define WAIT_MS(n) void wait##n##ms(void) { MilliSleep(n); }
WAIT_US(1) WAIT_US(2) WAIT_US(3) WAIT_US(4) WAIT_US(5) WAIT_US(10) WAIT_US(20) WAIT_US(30) WAIT_US(40)
WAIT_US(50) WAIT_US(100) WAIT_US(200) WAIT_US(300) WAIT_US(400) WAIT_US(500)
WAIT_MS(1) WAIT_MS(2) WAIT_MS(3) WAIT_MS(4) WAIT_MS(5) WAIT_MS(10) WAIT_MS(20) WAIT_MS(30) WAIT_MS(40)
WAIT_MS(50) WAIT_MS(100) WAIT_MS(200) WAIT_MS(300) WAIT_MS(400) WAIT_MS(500) WAIT_MS(1000)

/* COMP block and vendor TIM3 (APB1 + 0x400; our "TIM3" above is the vendor's TIM4) */
#define COMP       0x40002400u
#define CMP1_CTRL  R32(COMP + 0x10)
#define CMP_INVREF R32(COMP + 0x40)
#define TC         0x40000400u
#define TC_CR1     R32(TC + 0x00)
#define TC_STS     R32(TC + 0x10)
#define TC_EGR     R32(TC + 0x14)
#define TC_CCMR1   R32(TC + 0x18)
#define TC_CCER    R32(TC + 0x20)
#define TC_CNT     R32(TC + 0x24)
#define TC_PSC     R32(TC + 0x28)
#define TC_ARR     R32(TC + 0x2C)
#define TC_CCR1    R32(TC + 0x34)
#define APB1_TC    0x0002u
#define APB1_COMP  0x0100u
#define TC_C1SEL   (1u << 11)                           /* CH1 input from the comparator */

uint32_t ct_cap_charge(uint8_t channel, uint8_t ddr, uint16_t max_ovf)
{
    CMP_INVREF = 1u | (uint32_t)CT_COMP_VREF << 1;      /* VREF1 on */
    /* EN, INM = VREF1 (7), INP = PA1/PA3/PA6 (0/1/2 = TP1..TP3), OUTTRG = TIM3_IC1 (6) */
    CMP1_CTRL = 1u | 7u << 1 | (uint32_t)channel << 4 | 6u << 7;
    TC_CR1 = TC_C1SEL;
    TC_PSC = 0;
    TC_ARR = 0xFFFF;
    TC_CCMR1 = 1;                                       /* CC1 = input TI1, no filter */
    TC_CCER = 1;                                        /* capture enabled, rising edge */
    TC_EGR = 1;
    TC_CNT = 0;
    ct_wait_us(20);                                     /* comparator start-up (5 us) */
    TC_STS = 0;
    /* new probe pin modes: the released pins go to analog (high impedance) in one register write */
    uint32_t mode = GP_MODE(GPIOA);
    for (unsigned i = 0; i < 3; i++)
        if (!(ddr >> i & 1))
            mode |= 3u << 2 * a_pins[i].pin;
    a_ddr = ddr;
    uint32_t sts, ovf = 0;
    TC_CR1 = TC_C1SEL | 1;                              /* start timer */
    GP_MODE(GPIOA) = mode;                              /* start charging */
    for (;;) {
        sts = TC_STS;
        if (sts & 2)                                    /* captured */
            break;
        if (sts & 1) {                                  /* overflow every 1.37 ms */
            TC_STS = ~1u;
            ovf++;
            if (ovf == max_ovf)
                break;
            if (!(ovf & 255))
                wdt_feed();
        }
    }
    TC_CR1 = TC_C1SEL;                                  /* stop */
    uint32_t ticks = TC_CCR1 & 0xFFFF;
    if ((sts & 2) && (TC_STS & 1) && ticks < 0x8000)    /* overflow just before the capture */
        ovf++;
    TC_STS = 0;
    TC_CCER = 0;
    CMP1_CTRL = 0;
    CMP_INVREF = 0;
    relay_update();
    return ticks | ovf << 16;
}

/* ---- ESR (cap.c MeasureESR) ----
 * Upstream samples the voltage at the cap in the middle of a 2-4 us current pulse via Rl, timed to the AVR
 * ADC clock. Here: sample code 0 ends its sampling ESR_KS delay-loop runs after the start command (measured
 * with #E on the meter: 18; codes 1-4: 22/31/41/55), so the measuring pulse is switched off right then.
 * Each measuring pulse follows a reverse pre-charge of the same length, so the cap's own voltage is back
 * at zero at the sampling point (upstream: the negative half-pulse); forward and reverse alternate.
 * Pulses are GPIO mode writes only (levels set before), with interrupts off. */
#define ESR_ST 0
uint8_t ct_esr_ks = 18, ct_esr_kp = 19;          /* sampling end / pre-charge length (delay-loop runs) */

static void esr_delay(uint32_t n)
{
    __asm volatile(".syntax unified\n1: subs %0, %0, #1\n bne 1b" : "+l"(n) : : "cc");
}

static void modes(uint32_t *a, uint32_t *b, const struct vpin *x, const struct vpin *y)
{
    const struct vpin *p[2] = {x, y};
    for (int i = 0; i < 2; i++) {
        uint32_t *m = p[i]->port == GPIOA ? a : b;
        *m = (*m & ~(3u << 2 * p[i]->pin)) | (uint32_t)MODE_OUT << 2 * p[i]->pin;
    }
}

/* The end-of-conversion flag is no help here (still set from earlier conversions: the "one conversion
   late" of adc.c), but 30 us after the start the data register holds this conversion (#E on the meter) */
static uint32_t adc_wait(void)
{
    ct_wait_us(30);
    return ADC_DAT & 0xFFF;
}

uint8_t ct_esr_sums(uint8_t id1, uint8_t id2, uint32_t *sum1, uint32_t *sum2)
{
    const struct vpin *p1 = &a_pins[id1], *p2 = &a_pins[id2], *l1 = &r_pins[2 * id1], *l2 = &r_pins[2 * id2];
    uint8_t ch1 = adc_ch[id1], ch2 = adc_ch[id2];
    R_DDR_set(0);
    R_PORT_set(0);
    ADC_PORT_set(0);
    ADC_DDR_set(0);                                     /* all probe pins analog, relay open */
    uint32_t a0 = GP_MODE(GPIOA), b0 = GP_MODE(GPIOB);
    GP_BSC(GPIOA) = 1u << (p1->pin + 16) | 1u << (p2->pin + 16);   /* levels: probe pins low, Rl high */
    GP_BSC(l1->port) = 1u << l1->pin;
    GP_BSC(l2->port) = 1u << l2->pin;
    uint32_t fa = a0, fb = b0, ra = a0, rb = b0, sa1 = a0, sb1 = b0, sa3 = a0, sb3 = b0;
    modes(&fa, &fb, p1, l2);                            /* forward: Gnd -- probe-1 / probe-2 -- Rl -- Vcc */
    modes(&ra, &rb, p2, l1);                            /* reverse: Gnd -- probe-2 / probe-1 -- Rl -- Vcc */
    modes(&sa1, &sb1, p1, l1);                          /* U at RiL of probe-1 (probe-2 HiZ) */
    modes(&sa3, &sb3, p2, l2);                          /* U at RiL of probe-2 (probe-1 HiZ) */
    uint32_t s1 = 1, s2 = 1, ks = ct_esr_ks, kp = ct_esr_kp;
    uint8_t ok = 1;
    for (unsigned n = 0; n < 255; n++) {
        uint32_t u[4];
        for (int i = 0; i < 4; i++) {
            int fwd = !(i & 2);
            adc_select(i == 0 || i == 3 ? ch1 : ch2, ESR_ST);
            if (!(i & 1)) {                             /* U_1 / U_3: static, no current through the DUT */
                GP_MODE(GPIOA) = fwd ? sa1 : sa3;
                GP_MODE(GPIOB) = fwd ? sb1 : sb3;
                ct_wait_us(5);
                ADC_CR2 |= 0x500000;
                u[i] = adc_wait();
                GP_MODE(GPIOA) = a0;
                GP_MODE(GPIOB) = b0;
            } else {                                    /* U_2 / U_4: pre-charge, then the measuring pulse */
                uint32_t pa = fwd ? ra : fa, pb = fwd ? rb : fb, ma = fwd ? fa : ra, mb = fwd ? fb : rb;
                __asm volatile("cpsid i");
                GP_MODE(GPIOA) = pa;
                GP_MODE(GPIOB) = pb;
                esr_delay(kp);
                GP_MODE(GPIOA) = ma;
                GP_MODE(GPIOB) = mb;
                ADC_CR2 |= 0x500000;
                esr_delay(ks);
                GP_MODE(GPIOA) = a0;
                GP_MODE(GPIOB) = b0;
                __asm volatile("cpsie i");
                u[i] = adc_wait();
            }
        }
        s1 += u[0] + u[2];
        s2 += u[1] + u[3];
        if (!(n & 31))
            wdt_feed();
    }
    GP_MODE(GPIOA) = a0;
    GP_MODE(GPIOB) = b0;
    R_PORT_set(0);                                      /* Rl levels back to low (pins still analog) */
    ADC_PORT_set(0);
    *sum1 = s1;
    *sum2 = s2;
    return ok;
}

void ct_hal_init(void)
{
    RCC_APB1EN |= APB1_TIM3 | APB1_TC | APB1_COMP;
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
