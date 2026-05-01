/* Linker script for TM4C123GH6PM — 256KB flash, 32KB SRAM */

ENTRY(ResetISR)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    /* Vector table — must be first in flash */
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    /* Code and read-only data */
    .text :
    {
        *(.text*)
        *(.rodata*)
    } > FLASH

    .ARM.extab : { *(.ARM.extab*) } > FLASH
    .ARM.exidx : { *(.ARM.exidx*) } > FLASH

    /* Initialised data: stored in flash, copied to RAM by ResetISR */
    .data :
    {
        __data_start__ = .;
        *(.data .data.*)
        __data_end__ = .;
    } > RAM AT > FLASH

    /* LMA of .data — used by ResetISR copy loop */
    __data_load_start = LOADADDR(.data);

    /* Zero-initialised data */
    .bss :
    {
        __bss_start__ = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end__ = .;
    } > RAM

    /DISCARD/ : { *(.eh_frame) }
}
