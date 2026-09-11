    .section .text
    .global _start
_start:
    move.l %sp,0x4000f004
    move.w #0x2700,%sr
    move.l #0x4000fc00,%sp
    move.l 0x8000f000,%d0
    cmpi.l #0x11223344,%d0
    bne failed
    move.l #0x88776655,%d0
    move.l %d0,0x4000f000
    move.l #0x55667788,%d0
    move.l %d0,0x07001004
    move.l 0x07001000,%d0
    addq.l #1,%d0
    move.l %d0,0x07001000
progress:
    move.l 0x07001008,%d0
    addq.l #1,%d0
    move.l %d0,0x07001008
    bra progress
failed:
    move.l #0xffffffff,%d0
    move.l %d0,0x07001000
    bra failed
