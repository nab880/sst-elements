/* link_m68k_bsp_startup.ld -- BSP-shaped layout for the startup-path
 * validation firmware (A4). Mirrors raptor-bsp memory_map.ld / app_core1.ld
 * shape as closely as the QEMU mcf5208evb harness allows:
 *
 *   - The vector table (.vectorram) is placed FIRST, so its address is the
 *     VBR the startup code programs (ColdFire VBR forces 1 MB alignment;
 *     0x40000000 is the SDRAM base and is 1 MB aligned).
 *   - .data is linked IN PLACE in RAM (no separate LMA/VMA split), matching the
 *     BSP fact that crt0 does no ROM->RAM copy.
 *   - .bss is NOLOAD and is NOT cleared by startup (the BSP fact: no .bss
 *     zeroing loop). Under QEMU the SDRAM backing is zero-filled at reset, so a
 *     correct BSP-shaped image observes zeroed .bss without a clear loop; the
 *     firmware verifies this at runtime.
 *
 * QEMU `-kernel` loads the ELF and sets PC to _start.
 */
OUTPUT_ARCH(m68k)
ENTRY(_start)

__INTERRUPT_VECTOR = 0x40000000;   /* VBR target = load base (1 MB aligned) */

SECTIONS {
    . = 0x40000000;

    /* Vector table at VBR. Only entries 0/1/2 are populated by the source;
     * the rest are zero (NULL), exactly like the stock cf-isrs.c table. */
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
