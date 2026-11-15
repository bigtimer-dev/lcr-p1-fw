/* SPDX-License-Identifier: EUPL-1.2 -- LCR-P1 */
#ifndef COMPAT_AVR_SLEEP_H
#define COMPAT_AVR_SLEEP_H
#define set_sleep_mode(m) ((void)(m))
#define sleep_mode() ((void)0)
#define sleep_enable() ((void)0)
#define sleep_disable() ((void)0)
#define sleep_cpu() ((void)0)
#endif
