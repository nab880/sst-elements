OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
    . = 0x80000000;
    /* QEMU virt `-bios none -kernel` jumps to the fixed load address
     * 0x80000000, NOT the ELF entry. The boot stub MUST be first so the
     * reset jump lands on _start (.text.boot), not an arbitrary function. */
    .text   : { KEEP(*(.text.boot)) *(.text*) *(.rodata*) }
    .data   : { *(.data*)               }
    .bss    : { *(.bss*)    *(COMMON)   }
    . = ALIGN(16);
    _stack_top = . + 0x4000;
}
