OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(reset_handler)
SECTIONS {
    . = 0x00000000;
    .vectors : { *(.vectors) }  /* Cortex-M vector table must be at 0x0 */
    .text    : { *(.text*) *(.rodata*) }
    . = 0x20000000;
    .data    : { *(.data*) }
    .bss     : { *(.bss*) *(COMMON) }
}
