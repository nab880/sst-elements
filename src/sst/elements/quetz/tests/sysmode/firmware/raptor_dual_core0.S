    .section .text
    .global _start
_start:
    move.w #0x2700,%sr
    move.l #0x8000fc00,%sp
    move.l #0x04,%d0
    move.b %d0,0xfc064008
    move.l #0,%d0
    move.l %d0,0x07001000
    move.l #0,%d0
    move.l %d0,0x07001004
    move.l #0,%d0
    move.l %d0,0x07001008
    move.l #0x11223344,%d0
    move.l %d0,0x8000f000
    move.l #0xdeadbeef,%d0
    move.l %d0,0x4000f000
    bsr delay
    moveq #49,%d7
    move.l 0x07001000,%d0
    bne failed
    /* A high input-configured pin must not release the secondary. */
    move.l #0x0080,%d0
    move.w %d0,0xfc084018
    move.l #0x8080,%d0
    move.w %d0,0xfc084010
    bsr delay
    moveq #50,%d7
    move.l 0x07001000,%d0
    bne failed
    move.l #0,%d0
    move.w %d0,0xfc084018
wait_first:
    move.l 0x07001000,%d0
    cmpi.l #1,%d0
    bne wait_first
    move.l 0x07001004,%d0
    cmpi.l #0x55667788,%d0
    bne wait_first
    moveq #51,%d7
    move.l 0x4000f004,%d0
    cmpi.l #0x4000fc00,%d0
    bne failed
    moveq #52,%d7
    move.l 0x4000f000,%d0
    cmpi.l #0x88776655,%d0
    bne failed
    moveq #53,%d7
    move.l 0x8000f000,%d0
    cmpi.l #0x11223344,%d0
    bne failed
wait_progress:
    move.l 0x07001008,%d0
    beq wait_progress
    move.l #0x8000,%d0
    move.w %d0,0xfc084010
    move.l 0x07001008,%d3
    bsr delay
    move.l 0x07001008,%d0
    moveq #54,%d7
    cmp.l %d3,%d0
    bne failed
    move.l #0x8080,%d0
    move.w %d0,0xfc084010
wait_second:
    move.l 0x07001000,%d0
    cmpi.l #2,%d0
    bne wait_second
    move.l 0x4000f004,%d0
    cmpi.l #0x4000fc00,%d0
    bne failed
    move.l #0x8000,%d0
    move.w %d0,0xfc084010
    lea passed_text,%a0
    bra emit
failed:
    move.b %d7,0xfc06400c
    lea failed_text,%a0
emit:
    move.b (%a0)+,%d0
    beq idle
    move.b %d0,0xfc06400c
    bra emit
idle:
    bra idle
delay:
    move.l #1000000,%d2
1:  subq.l #1,%d2
    bne 1b
    rts
    .section .rodata
passed_text: .asciz "MULTICORE PASS\n"
failed_text: .asciz "MULTICORE FAIL\n"
