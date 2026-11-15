/* SPDX-License-Identifier: EUPL-1.2 -- LCR-P1 */
#ifndef COMPAT_AVR_INTERRUPT_H
#define COMPAT_AVR_INTERRUPT_H
#define cli() __asm__ volatile("cpsid i" ::: "memory")
#define sei() __asm__ volatile("cpsie i" ::: "memory")
#endif
