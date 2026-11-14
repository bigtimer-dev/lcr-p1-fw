/* SPDX-License-Identifier: EUPL-1.2
 * Vector table and reset. The image is linked at 0x08001800, behind FNIRSI's bootloader, which
 * starts the app through this table (the stock app never sets VTOR either).
 */
#include <stdint.h>
#include "board.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);
void SysTick_Handler(void);

void Reset_Handler(void)
{
    power_hold();                               /* before anything slow: user may release TEST */
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
    SysTick_Handler,
    /* IRQ0..31: none used yet */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
};
