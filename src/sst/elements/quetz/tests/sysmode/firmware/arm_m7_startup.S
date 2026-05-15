/* arm_m7_startup.S — minimal Cortex-M7 vector table + reset entry */
    .syntax unified
    .thumb

    .section .vectors, "a"
    .align  2
    .long   0x20008000          /* initial MSP: top of 32 KiB of SRAM */
    .long   reset_handler + 1  /* Reset (Thumb bit already set via +1) */

    .text
    .type reset_handler, %function
    .global reset_handler
reset_handler:
    /* stack is already set from the vector table */
    bl  uart_main
1:  wfi
    b   1b
    .size reset_handler, . - reset_handler
