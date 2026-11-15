/* SPDX-License-Identifier: EUPL-1.2
 *
 *  ComponentTester board configuration for the FNIRSI LCR-P1 (Nations N32G032C8L7)
 *  - takes the place of config_328.h etc. in the upstream sources
 *  - the AVR-style ports R_* / ADC_* are virtual 8-bit ports implemented by src/ct_hal.c:
 *    the converted ComponentTester code writes them through R_DDR_set() etc.
 */

#ifndef CONFIG_LCRP1_H
#define CONFIG_LCRP1_H

#include "ct_hal.h"

extern uint16_t ct_adc_fs_mv;            /* ADC full scale in mV (src/ct_glue.c) */

/* debug output inserted by tools/ctport.py (build with make CT_DEBUG=1) */
#ifdef CT_DEBUG
  void uprintf(const char *fmt, ...);
  #define CT_DBG(...) uprintf(__VA_ARGS__)
#else
  #define CT_DBG(...) ((void)0)
#endif

/* ************************************************************************
 *   display module: ST7735S-type 128x128 panel on hardware SPI
 *   (the LCD_* functions are implemented in src/ct_display.c on top of src/lcd.c)
 * ************************************************************************ */

#define LCD_ST7735                      /* display controller ST7735 */
#define LCD_GRAPHIC                     /* graphic display */
#define LCD_COLOR                       /* color display */
#define LCD_SPI                         /* SPI interface */
#define LCD_DOTS_X       128            /* number of horizontal dots */
#define LCD_DOTS_Y       128            /* number of vertical dots */
#define FONT_8X16_ISO8859_2_HF          /* 8x16 font (metrics only: glyphs come from src/font_data.c) */
#define SYMBOLS_30X32_HF                /* 30x32 symbols */
#define SPI_HARDWARE                    /* hardware SPI (src/lcd.c) */


/* ************************************************************************
 *   probes and test pins (virtual ADC port, bit numbers as on the ATmega328)
 * ************************************************************************ */

#define ADC_PORT         ADC_PORT_       /* only for #ifdef tests; access via ADC_PORT_set() */
#define ADC_DDR          ADC_DDR_
#define ADC_PIN          ADC_PIN_get()
#define TP1              0               /* probe #1: PA1, ADC ch1 */
#define TP2              1               /* probe #2: PA3, ADC ch3 */
#define TP3              2               /* probe #3: PA6, ADC ch6 */

#define TP_ZENER         3               /* Zener check, 10:1 divider: PA2, ADC ch2 */
#define TP_REF           4               /* 2.5V reference: PA0, ADC ch0; DDR bit = discharge relay */
#define TP_BAT           5               /* battery, 1:1 divider (x2): PB0, ADC ch8 */


/* ************************************************************************
 *   probe resistors (virtual R port, bit numbers as on the ATmega328)
 * ************************************************************************ */

#define R_PORT           R_PORT_
#define R_DDR            R_DDR_
#define R_RL_1           0               /* Rl (680R) for probe #1: PA4 */
#define R_RH_1           1               /* Rh (470k) for probe #1: PA5 */
#define R_RL_2           2               /* Rl (680R) for probe #2: PA7 */
#define R_RH_2           3               /* Rh (470k) for probe #2: PB1 */
#define R_RL_3           4               /* Rl (680R) for probe #3: PB2 */
#define R_RH_3           5               /* Rh (470k) for probe #3: PB10 */


/* ************************************************************************
 *   power control, test button (handled by src/board.c; names for upstream code)
 * ************************************************************************ */

#define POWER_CTRL       8               /* PA8 */
#define TEST_BUTTON      13              /* PC13, low active */


/* ************************************************************************
 *   ADC reference selection (no switchable reference: always VDD)
 * ************************************************************************ */

#define ADC_REF_VCC           0x00
#define ADC_REF_BANDGAP       0x40
#define ADC_REF_MASK          0x40
#define ADC_CHAN_BANDGAP      0x0e
#define ADC_CHAN_MASK         0x0f


/* ************************************************************************
 *   MCU specific values
 * ************************************************************************ */

/* estimated internal resistance of port to GND / VCC (in 0.1 Ohms): measured by self-adjustment */
#define R_MCU_LOW           200
#define R_MCU_HIGH          220
#define COMPARATOR_OFFSET   0
/* 64kB flash, NV data in flash (emulated EEPROM), 16kB RAM */
#define RES_FLASH           64
#define RES_EEPROM          1
#define RES_RAM             8

#endif
