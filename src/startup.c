/* SPDX-License-Identifier: EUPL-1.2
 * Vector table and reset. The image is linked at 0x08001800, behind FNIRSI's bootloader. The core
 * (Cortex-M0) has no VTOR; the bootloader points the N32's vector remap register (0x40024C30) at this
 * table, then checks the initial SP (must be < 0x20002000) and jumps to the reset vector.
 */
#include <stdint.h>
#include "board.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);
void TIM6_IRQHandler(void);

void Reset_Handler(void)
{
    power_hold();                               /* before anything slow: user may release TEST */
    wdt_start();                                /* from here on a hang switches the meter off */
    uint32_t *s = &_sidata, *d = &_sdata;
    while (d < &_edata)
        *d++ = *s++;
    for (d = &_sbss; d < &_ebss;)
        *d++ = 0;
    clock_init();
    main();
    for (;;)
        ;
}

static void Default_Handler(void)
{
    for (;;)
        ;
}

__attribute__((section(".vectors"), used))
const void *const vectors[48] = {
    &_estack,
    Reset_Handler,
    Default_Handler,        /* NMI */
    Default_Handler,        /* HardFault */
    0, 0, 0, 0, 0, 0, 0,
    Default_Handler,        /* SVCall */
    0, 0,
    Default_Handler,        /* PendSV */
    Default_Handler,        /* SysTick (not used) */
    /* IRQ0..31: IRQ20 = TIM6 tick */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    TIM6_IRQHandler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
};
