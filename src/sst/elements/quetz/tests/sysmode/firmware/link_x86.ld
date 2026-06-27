OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(kernel_main)
SECTIONS {
    . = 0x100000;           /* standard multiboot load address (1 MiB) */
    .multiboot : { *(.multiboot) }
    .text      : { *(.text*)  *(.rodata*) }
    .data      : { *(.data*)  }
    .bss       : { *(.bss*)   *(COMMON) }
}
