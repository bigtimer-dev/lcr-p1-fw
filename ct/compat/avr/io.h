/* SPDX-License-Identifier: EUPL-1.2 -- LCR-P1: AVR compatibility for the ComponentTester sources */
#ifndef COMPAT_AVR_IO_H
#define COMPAT_AVR_IO_H
#include <stdint.h>
#ifndef _BV
#define _BV(b) (1u << (b))
#endif
#endif
