/* SPDX-License-Identifier: EUPL-1.2
 * N32G032C8L7: flash 0x08000000 64 KB (FNIRSI bootloader 0x08000000-0x080017FF), RAM 16 KB.
 * The app stays below 0x0800FC00: the last flash page holds the stock firmware's calibration
 * record (0x0800FFD0). RAM limited to 8 KB until 16 KB is confirmed on hardware.
 */
MEMORY {
  FLASH (rx) : ORIGIN = 0x08001800, LENGTH = 0xE400
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 8K
}
/* The FNIRSI bootloader only starts an app whose initial SP satisfies (SP & 0x0FFFFFFF) < 0x2000,
   i.e. SP <= 0x20001FFF (it stays in update mode otherwise, holding the power latch). */
_estack = 0x20001FF8;
ENTRY(Reset_Handler)
SECTIONS {
  .text : {
    KEEP(*(.vectors))
    *(.text*) *(.rodata*)
    . = ALIGN(4);
  } > FLASH
  .data : {
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH
  _sidata = LOADADDR(.data);
  .bss (NOLOAD) : {
    _sbss = .;
    *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM
  ASSERT(_ebss + 1024 <= _estack, "less than 1 KB left for the stack")
  ASSERT((_estack & 0x0FFFFFFF) < 0x2000, "bootloader refuses an initial SP >= 0x20002000")
  /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.ARM.attributes) }
}
