/* BSP-shaped RAM layout: aligned vectors, in-place data and NOLOAD BSS without startup clearing. */
OUTPUT_ARCH(m68k)
ENTRY(_start)

__INTERRUPT_VECTOR = 0x40000000;   /* VBR target = load base (1 MB aligned) */

SECTIONS {
    . = 0x40000000;

    /* Vectors 0..8 are live; 9..255 remain zero, matching BSP 5.7. */
    .vectorram : { KEEP(*(.vectorram)) }

    .text : {
        KEEP(*(.text.boot))
        *(.text*)
        *(.rodata*)
    }

    /* .data linked in place (VMA == LMA); no separate load region to copy from. */
    .data : { *(.data*) }

    /* .bss is NOLOAD and deliberately not cleared by startup. */
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    __STACK = . + 0x4000;          /* initial supervisor SP (top of this block) */
    __stack_bottom = .;
}
