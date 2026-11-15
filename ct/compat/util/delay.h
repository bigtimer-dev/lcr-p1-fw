/* SPDX-License-Identifier: EUPL-1.2 -- LCR-P1 */
#ifndef COMPAT_UTIL_DELAY_H
#define COMPAT_UTIL_DELAY_H
#include "ct_hal.h"
#define _delay_us(n) ct_wait_us((uint16_t)(n))
#define _delay_ms(n) MilliSleep((uint16_t)(n))
#endif
