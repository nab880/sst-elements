| Exercise stock SR/RAMBAR/SP/VBR startup without copying data or clearing BSS.

	.equ SR_INIT,      0x2000
	.equ RAMBAR_CTL,   0xC05
	.equ VBR_CTL,      0x801
	.equ RAMBAR_FLAGS, 0x201        | (1<<9) backdoor-enable | (1<<0) valid

	| RAMBAR base for the mcf5208evb harness.
	.equ RAMBAR_BASE,  0x40000000

	.extern kernel_main
	.extern __STACK
	.extern __INTERRUPT_VECTOR
	.extern g_startup_fault_flag
	.extern g_startup_resume_pc

| ---------------------------------------------------------------------------
	.section .text.boot,"ax"
	.global _start
	.type _start,@function
_start:
	| 1. SR := supervisor, IPL mask 0
	move.l	#SR_INIT, %d0
	move.w	%d0, %sr

	| Set RAMBAR valid/backdoor-enable before installing the stack.
	move.l	#(RAMBAR_BASE + RAMBAR_FLAGS), %d0
	movec	%d0, &RAMBAR_CTL

	| 3. SP := __STACK
	move.l	#__STACK, %sp

	| 4. VBR := __INTERRUPT_VECTOR  (control register 0x801)
	move.l	#__INTERRUPT_VECTOR, %d1
	movec	%d1, &VBR_CTL

	| 5. jmp main (never returns)
	move.l	#kernel_main, %a0
	jmp	(%a0)
	.size _start,.-_start

| --------------------------------------------------------------------------- Access-error handler (vector 2).
	.section .text
	.align 2
	.global cf_startup_fault
	.type cf_startup_fault,@function
cf_startup_fault:
	move.l	%a0,-(%sp)
	move.l	%d0,-(%sp)
	moveq	#1,%d0
	lea	g_startup_fault_flag,%a0
	move.l	%d0,(%a0)
	lea	g_startup_resume_pc,%a0
	move.l	(%a0),%d0
	move.l	%d0,12(%sp)             | overwrite saved PC (past 2 saves + frame word0)
	move.l	(%sp)+,%d0
	move.l	(%sp)+,%a0
	rte
	.size cf_startup_fault,.-cf_startup_fault

| --------------------------------------------------------------------------- Vector table, cf-isrs.c shape: entries 0/1/2 live, rest zero (NULL).
	.section .vectorram,"a"
	.align 2
	.global _vect
_vect:
	.long	__STACK                 |  0 (0x000) initial supervisor SP
	.long	_start                  |  1 (0x004) initial PC
	.long	cf_startup_fault        |  2 (0x008) access error
	| entries 3..255 left as zero fill (NULL), like the stock table
	.space	(256-3)*4, 0
	.size _vect,.-_vect
