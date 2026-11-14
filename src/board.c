/* SPDX-License-Identifier: EUPL-1.2
 * Clock, GPIO, power latch, relay, buzzer, keys.
 */
#include "board.h"
#include "hw.h"

volatile uint32_t g_ms;

void SysTick_Handler(void)
{
    g_ms++;
}

/* Same steps as the stock SystemInit: HSI, flash latency 2 + prefetch, PLL (HSI) -> 48 MHz,
   AHB and APB2 undivided. Polls are bounded; on a dead clock we stay on HSI. */
void clock_init(void)
{
    RCC_CR |= 1;                                /* HSI on */
    RCC_CFGR &= 0x21FF0000u;
    RCC_CR &= 0xFEF6FFFFu;
    RCC_CR &= ~(1u << 18);
    RCC_CFGR &= 0xFE00FFFFu;
    RCC_CFGR2 = 7u << 11;
    RCC_CIR = 0xBFu << 16;
    FLASH_ACR |= 0x10;                          /* prefetch */

    for (unsigned n = 0; !(RCC_CR & 2) && n < 0x500; n++)
        ;
    if (!(RCC_CR & 2))
        return;
    FLASH_ACR = (FLASH_ACR & ~3u) | 2;          /* 2 wait states for 48 MHz */
    RCC_CFGR &= ~(3u << 22);
    RCC_CFGR &= 0xFEC0FFFFu;
    RCC_CFGR |= 3u << 16;                       /* PLL source / multiplier as stock */
    RCC_CR |= 1u << 24;                         /* PLL on */
    for (unsigned n = 0; !(RCC_CR & (1u << 25)); n++)
        if (n > 100000)
            return;
    RCC_CFGR = (RCC_CFGR & ~3u) | 2;            /* system clock = PLL */
    for (unsigned n = 0; (RCC_CFGR & 0xC008u) != 0x4000u; n++)
        if (n > 100000)
            break;
    RCC_CFGR &= ~0xF0u;                         /* AHB /1 */
    RCC_CFGR &= ~(7u << 11);                    /* APB2 /1 */
}

void gpio_mode(uint32_t port, unsigned pin, unsigned mode)
{
    GP_MODE(port) = (GP_MODE(port) & ~(3u << 2 * pin)) | mode << 2 * pin;
}

void gpio_pull(uint32_t port, unsigned pin, unsigned pull)
{
    GP_PULL(port) = (GP_PULL(port) & ~(3u << 2 * pin)) | pull << 2 * pin;
}

void gpio_af(uint32_t port, unsigned pin, unsigned af)
{
    volatile uint32_t *r = pin < 8 ? &GP_AFL(port) : &GP_AFH(port);
    unsigned s = 4 * (pin & 7);
    *r = (*r & ~(15u << s)) | af << s;
}

void gpio_set(uint32_t port, unsigned pin, int on)
{
    GP_BSC(port) = on ? 1u << pin : 1u << (pin + 16);
}

int gpio_get(uint32_t port, unsigned pin)
{
    return GP_IN(port) >> pin & 1;
}

void power_hold(void)
{
    RCC_APB2EN |= APB2_GPIOA;
    gpio_set(PIN_POWER, 1);                     /* level first, then output: no glitch */
    gpio_pull(PIN_POWER, PULL_UP);
    gpio_mode(PIN_POWER, MODE_OUT);
}

void power_off(void)
{
    gpio_set(PIN_POWER, 0);
    for (;;)
        ;                                       /* power fades (or USB keeps us here) */
}

void delay_ms(uint32_t ms)
{
    uint32_t t = g_ms;
    while (g_ms - t < ms)
        ;
}

void relay(int on)
{
    gpio_set(PIN_RELAY, on);
}

static uint32_t beep_end;

void beep(unsigned ms)
{
    beep_end = g_ms + ms;
    T1_CCER = 0x13;                             /* CH1 (buzzer) on, CH2 (backlight) on */
}

void beep_poll(void)
{
    if ((T1_CCER & 1) && (int32_t)(g_ms - beep_end) >= 0)
        T1_CCER = 0x12;
}

static void tim1_init(void)
{
    /* as stock: 48 MHz / 48 / 370 = 2.7 kHz; CH1 buzzer PWM 50/370, CH2 backlight 100 % */
    T1_CR1 = 0;
    T1_ARR = 369;
    T1_PSC = 47;
    T1_RCR = 0;
    T1_EGR = 1;
    T1_CR1 = 0x8800;
    T1_CR2 = 0x14;
    T1_CCMR1 = 0x6868;                          /* CH1, CH2 PWM mode 1, preload */
    T1_CCR1 = 50;
    T1_CCR2 = 0xA8C;                            /* > ARR: always on */
    T1_CCER = 0x12;                             /* CH1 off, CH2 on */
    T1_BDTR = 0x8000;                           /* main output enable */
    T1_CR1 |= 0x81;
    gpio_af(GPIOB, 8, 5);
    gpio_af(GPIOB, 9, 5);
    gpio_mode(GPIOB, 8, MODE_AF);
    gpio_mode(GPIOB, 9, MODE_AF);
}

void board_init(void)
{
    RCC_APB2EN |= APB2_AFIO | APB2_GPIOA | APB2_GPIOB | APB2_GPIOC | APB2_GPIOF | APB2_SPI |
                  APB2_TIM1 | APB2_UART1;

    SYST_RVR = 48000 - 1;                       /* 1 ms at 48 MHz core clock */
    SYST_CVR = 0;
    SYST_CSR = 7;                               /* core clock, interrupt, enable */

    gpio_set(PIN_RELAY, 0);                     /* relay off: probes shorted (safe) */
    gpio_mode(PIN_RELAY, MODE_OUT);
    gpio_set(PIN_HV, 0);                        /* HV boost off */
    gpio_mode(PIN_HV, MODE_OUT);

    gpio_pull(PIN_KEY_TEST, PULL_UP);
    gpio_mode(PIN_KEY_TEST, MODE_IN);
    gpio_pull(PIN_MODE_IR, PULL_UP);
    gpio_mode(PIN_MODE_IR, MODE_IN);
    gpio_pull(PIN_KEY_ZENER, PULL_UP);
    gpio_mode(PIN_KEY_ZENER, MODE_IN);

    tim1_init();
}

/* keys: sampled from the main loop, 20 ms debounce; TEST held 1 s = TEST_LONG */
static uint8_t st_test, st_zener, long_sent, armed;
static uint32_t t_test, t_zener, t_down;

int key_test_down(void) { return !gpio_get(PIN_KEY_TEST); }
int key_zener_down(void) { return !gpio_get(PIN_KEY_ZENER); }
int mode_ir(void) { return gpio_get(PIN_MODE_IR); }

int key_poll(void)
{
    uint32_t now = g_ms;
    int ev = KEY_NONE;
    int t = key_test_down(), z = key_zener_down();

    if (!armed) {                               /* TEST is still held from power-on: wait for release */
        if (!t && !z && now > 50)
            armed = 1, st_test = st_zener = 0, t_test = t_zener = now;
        return KEY_NONE;
    }
    if (t != st_test && now - t_test >= 20) {
        st_test = t, t_test = now;
        if (t)
            t_down = now, long_sent = 0;
        else if (!long_sent)
            ev = KEY_TEST;
    }
    if (st_test && !long_sent && now - t_down >= 1000)
        long_sent = 1, ev = KEY_TEST_LONG;
    if (z != st_zener && now - t_zener >= 20) {
        st_zener = z, t_zener = now;
        if (z && ev == KEY_NONE)
            ev = KEY_ZENER;
    }
    return ev;
}
