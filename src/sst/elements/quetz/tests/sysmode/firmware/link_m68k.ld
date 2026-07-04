OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS {
    . = 0x40000000;
    /* QEMU mcf5208evb has SDRAM at 0x40000000 and `-kernel` sets the CPU PC to
     * the ELF entry (_start), so unlike RISC-V virt there is no fixed load
     * address to match — ENTRY(_start) is authoritative. .text.boot is kept
     * first only for readability. */
    /* Exception vector table (coldfire_intc.h): ColdFire VBR forces 1 MB
     * alignment, and 0x40000000 (SDRAM base) is the natural spot. Empty —
     * and layout-neutral — for firmware that takes no interrupts. */
    .vectors : { KEEP(*(.vectors)) }
    .text   : { KEEP(*(.text.boot)) *(.text*) *(.rodata*) }
    .data   : { *(.data*)               }
    .bss    : { *(.bss*)    *(COMMON)   }
    . = ALIGN(16);
    _stack_top = . + 0x4000;
}
