    .section .text
    .global _start
_start:
    move.w #0x2700,%sr
    move.l #0x4000fc00,%sp
    move.l 0x71000000,%d0
    cmpi.l #0x11223344,%d0
    bne failed
    move.b 0x71000002,%d0
    cmpi.b #0x33,%d0
    bne failed
    move.l #0xaabbccdd,%d0
    move.l %d0,0x71000004
wait_ack:
    move.l 0x71000000,%d0
    cmpi.l #0x55667788,%d0
    bne wait_ack
    moveq #1,%d0
    move.l %d0,0x07001000
    move.l #0x5555,%d0
    bra finish
failed:
    move.l #0x3333,%d0
finish:
    move.l %d0,0x0700fff4
    stop #0x2700
    bra finish
