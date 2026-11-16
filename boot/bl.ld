/* SPDX-License-Identifier: EUPL-1.2
 * LCR-P1 bootloader: 0x08000000-0x080017FF (the FNIRSI loader's slot; apps start at 0x08001800).
 * The last 16 bytes hold struct bl_info (magic, version, size, CRC32), stamped by boot/mkinfo.py.
 * RAM: 8 KB; 0x20001FF8/FFC = app -> bootloader request words, never touched by the loader itself.
 */
MEMORY {
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 0x17F0
  BLINFO (r)  : ORIGIN = 0x080017F0, LENGTH = 16
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 0x1FF0
}
_estack = 0x20001FF0;
ENTRY(Reset_Handler)
SECTIONS {
  .text : {
    KEEP(*(.vectors))
    *(.text*) *(.rodata*)
    . = ALIGN(4);
  } > FLASH
  .blinfo : { KEEP(*(.blinfo)) } > BLINFO
  .data : { *(.data*) } > RAM AT > FLASH
  ASSERT(SIZEOF(.data) == 0, "bootloader: no initialised data (there is no copy loop)")
  .bss (NOLOAD) : {
    _sbss = .;
    *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM
  ASSERT(_ebss + 768 <= _estack, "bootloader: less than 768 bytes of stack")
  /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.ARM.attributes) }
}
