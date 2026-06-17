| coldfire_startup.S — reset stub for QEMU ColdFire mcf5208evb (big-endian m68k).
|
| QEMU `-kernel` loads the ELF and sets the CPU PC to the ELF entry (_start).
| The stack pointer (A7/%sp) is not guaranteed, so set it explicitly before
| calling C. After kernel_main returns, spin until the SST side ends the
| simulation (the TestFinisherRegionHandler halts the core on the sentinel
| store, then the Quetz launcher SIGTERMs QEMU).
|
| Shared by all ColdFire firmwares (coldfire_hello.c, coldfire_monitor.c).

	.section .text.boot,"ax"
	.global _start
	.type _start,@function
_start:
	move.l	#_stack_top, %sp
	jsr	kernel_main
1:	bra.b	1b
	.size _start,.-_start
