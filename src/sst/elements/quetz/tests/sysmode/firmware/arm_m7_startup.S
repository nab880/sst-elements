
    .syntax unified
    .thumb

    .section .vectors, "a"
    .align  2
    .long   0x20008000          
    .long   reset_handler + 1  

    .text
    .type reset_handler, %function
    .global reset_handler
reset_handler:
    
    bl  uart_main
1:  wfi
    b   1b
    .size reset_handler, . - reset_handler
