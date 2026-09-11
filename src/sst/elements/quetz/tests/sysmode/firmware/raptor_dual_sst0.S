    .section .text
    .global _start
_start:
    move.w #0x2700,%sr
    move.l #0x8000fc00,%sp
    moveq #4,%d0
    move.b %d0,0xfc064008
    moveq #0,%d0
    move.l %d0,0x07001000
    move.l %d0,0x71000004
    move.l #0x11223344,%d0
    move.l %d0,0x71000000
    move.l #0x8080,%d0
    move.w %d0,0xfc084010
wait_reply:
    move.l 0x71000004,%d0
    cmpi.l #0xaabbccdd,%d0
    bne wait_reply
    move.b 0x71000005,%d0
    cmpi.b #0xbb,%d0
    bne failed
    move.l #0x55667788,%d0
    move.l %d0,0x71000000
wait_done:
    move.l 0x07001000,%d0
    cmpi.l #1,%d0
    bne wait_done
    lea pass_text,%a0
    move.l #0x5555,%d3
    bra emit
failed:
    lea fail_text,%a0
    move.l #0x3333,%d3
emit:
    move.b (%a0)+,%d0
    beq finish
    move.b %d0,0xfc06400c
    bra emit
finish:
    move.l %d3,0x0700fff0
    stop #0x2700
    bra finish
    .section .rodata
pass_text: .asciz "QUETZ MULTICORE PASS\n"
fail_text: .asciz "QUETZ MULTICORE FAIL\n"
