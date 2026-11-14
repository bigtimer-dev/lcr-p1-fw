/* SPDX-License-Identifier: EUPL-1.2
 * FNIRSI LCR-P1 hardware: Nations N32G032C8L7 (Cortex-M0, 48 MHz, 64 KB flash, 16 KB RAM).
 * Addresses and bits as used by the stock firmware v1.1.0 (see ~/Work/lcr-p1/hwmap/HARDWARE.md).
 */
#ifndef HW_H
#define HW_H
#include <stdint.h>

#define R32(a) (*(volatile uint32_t *)(a))
#define R16(a) (*(volatile uint16_t *)(a))

/* RCC / flash */
#define RCC        0x40021000u
#define RCC_CR     R32(RCC + 0x00)    /* bit0 HSI on, bit1 HSI ready, bit24 PLL on, bit25 PLL ready */
#define RCC_CFGR   R32(RCC + 0x04)    /* [1:0] clock switch, [7:4] AHB div, [13:11] APB2 div */
#define RCC_CIR    R32(RCC + 0x08)
#define RCC_AHBEN  R32(RCC + 0x14)
#define RCC_APB2EN R32(RCC + 0x18)
#define RCC_APB1EN R32(RCC + 0x1C)
#define RCC_CFGR2  R32(RCC + 0x2C)
#define FLASH_ACR  R32(0x40022000u)   /* [1:0] latency, bit4 prefetch */

#define APB2_AFIO  0x0001u
#define APB2_GPIOA 0x0004u
#define APB2_GPIOB 0x0008u
#define APB2_GPIOC 0x0010u
#define APB2_GPIOF 0x0080u
#define APB2_SPI   0x0200u
#define APB2_TIM1  0x2000u
#define APB2_UART1 0x4000u
#define AHB_ADC    0x1000u

/* GPIO: 2-bit mode per pin (0 in, 1 out, 2 alt, 3 analog), pull 2-bit (1 up, 2 down) */
#define GPIOA 0x40010800u
#define GPIOB 0x40010C00u
#define GPIOC 0x40011000u
#define GPIOF 0x40011C00u
#define GP_MODE(p) R32((p) + 0x00)
#define GP_OTYPE(p) R32((p) + 0x04)
#define GP_PULL(p) R32((p) + 0x0C)
#define GP_IN(p)   R32((p) + 0x10)
#define GP_OUT(p)  R32((p) + 0x14)
#define GP_BSC(p)  R32((p) + 0x18)    /* low 16 bits set, high 16 bits clear */
#define GP_AFL(p)  R32((p) + 0x20)
#define GP_AFH(p)  R32((p) + 0x24)
#define GP_BC(p)   R32((p) + 0x28)
enum { MODE_IN, MODE_OUT, MODE_AF, MODE_ANALOG };
enum { PULL_NONE, PULL_UP, PULL_DOWN };

/* SPI (LCD, transmit only, 8 bit): 16-bit registers */
#define SPI        0x40012000u
#define SPI_CR1    R16(SPI + 0x00)
#define SPI_STS    R16(SPI + 0x08)    /* bit1 transmit empty, bit7 busy */
#define SPI_DAT    R16(SPI + 0x0C)
#define SPI_CRCPOLY R16(SPI + 0x10)
#define SPI_I2SCFG R16(SPI + 0x1C)

/* USART1 (USB serial via CH340): PA9 TX, PA10 RX, AF4 */
#define UART       0x40013800u
#define UART_STS   R32(UART + 0x00)   /* bit5 receive not empty, bit7 transmit empty */
#define UART_DAT   R32(UART + 0x04)
#define UART_BRR   R32(UART + 0x08)
#define UART_CR1   R32(UART + 0x0C)   /* bit13 enable, bit3 TX, bit2 RX */
#define UART_CR2   R32(UART + 0x10)
#define UART_CR3   R32(UART + 0x14)

/* TIM1: CH1 = buzzer, CH2 = LCD backlight (PB8/PB9 AF5) */
#define TIM1       0x40013400u
#define T1_CR1     R32(TIM1 + 0x00)
#define T1_CR2     R32(TIM1 + 0x04)
#define T1_EGR     R32(TIM1 + 0x14)
#define T1_CCMR1   R32(TIM1 + 0x18)
#define T1_CCER    R32(TIM1 + 0x20)
#define T1_PSC     R32(TIM1 + 0x28)
#define T1_ARR     R32(TIM1 + 0x2C)
#define T1_RCR     R32(TIM1 + 0x30)
#define T1_CCR1    R32(TIM1 + 0x34)
#define T1_CCR2    R32(TIM1 + 0x38)
#define T1_BDTR    R32(TIM1 + 0x44)

/* SysTick */
#define SYST_CSR   R32(0xE000E010u)
#define SYST_RVR   R32(0xE000E014u)
#define SYST_CVR   R32(0xE000E018u)

/* Board pins */
#define PIN_POWER   GPIOA, 8      /* power latch: 1 keeps the meter on */
#define PIN_RELAY   GPIOB, 3      /* 1 = relay on = probes released (idle: probes shorted together) */
#define PIN_HV      GPIOF, 6      /* Zener HV boost enable (never set in phase 1) */
#define PIN_KEY_TEST  GPIOC, 13   /* active low */
#define PIN_KEY_ZENER GPIOF, 7    /* active low */
#define PIN_MODE_IR   GPIOC, 14   /* 1 = slide switch on IR */
#define PIN_LCD_RST GPIOB, 4
#define PIN_LCD_DC  GPIOB, 5      /* 0 = command */
#define PIN_LCD_CS  GPIOB, 6      /* active low */

#endif
