/* Minimal linker script for TM4C123 (32KB RAM, 128KB flash) */

ENTRY(ResetISR)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

_estack = 0x20008000; /* end of RAM */

SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
  } > FLASH

.text :
  {
      KEEP(*(.isr_vector))    /* match startup_tm4c.s exactly */
      *(.text*)
      *(.rodata*)
  } > FLASH

  .ARM.extab   : { *(.ARM.extab* ) } > FLASH
  .ARM.exidx   : { *(.ARM.exidx* ) } > FLASH

  .data : AT(ADDR(.text) + SIZEOF(.text))
  {
    __data_start__ = .;
    *(.data .data.*)
    __data_end__ = .;
  } > RAM

  .bss :
  {
    __bss_start__ = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end__ = .;
  } > RAM

  .stack (NOLOAD) :
  {
    . = ALIGN(8);
    __stack_start__ = .;
    . = . + 0x400; /* 1KB stack reserved (adjust if needed) */
    __stack_end__ = .;
  } > RAM

  /DISCARD/ : { *(.eh_frame) }
}
