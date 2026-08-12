| coldfire_bsp_startup.S -- A4: reproduce the stock raptor-bsp startup path
| (crt0.c / cf-isrs.c) under Quetz, exactly, and hand off to kernel_main.
|
| Startup sequence, in the BSP's order (raptor-bsp src/startup/crt0.c):
|   1. SR    := 0x2000              supervisor mode, interrupt level mask 0
|   2. RAMBAR := __RAMBAR|(1<<9)|1  valid + backdoor-enable (movec ctrl 0xC05)
|   3. SP    := __STACK
|   4. VBR   := __INTERRUPT_VECTOR  (movec ctrl 0x801)
|   5. jmp main                     via move.l into %a0, jmp (%a0); never returns
|
| Negative facts the BSP relies on and this image preserves (verified in C):
|   - .bss is never zeroed by startup (no clear loop here).
|   - .data is never copied (linked in place; see the linker script).
|   - No CACR / ACR / chip-select setup.
|
| Vector table (.vectorram, cf-isrs.c shape): only entries 0/1/2 are live; the
| rest are zero (NULL). Entry 2 (access error) points at a handler that records
| the fault and reports it, so the "stray vector = loud failure" property can be
| checked by deliberately taking one.

	.equ SR_INIT,      0x2000
	.equ RAMBAR_CTL,   0xC05
	.equ VBR_CTL,      0x801
	.equ RAMBAR_FLAGS, 0x201        | (1<<9) backdoor-enable | (1<<0) valid

	| RAMBAR base for the mcf5208evb harness. The real BSP uses
	| __RAMBAR_START (per-core RAM); here we point at the SDRAM base, which
	| is what matters for exercising the movec-to-RAMBAR path.
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

	| 2. RAMBAR := base | backdoor-enable | valid  (control register 0xC05).
	| Use the numeric control-register form (movec Rc,&<num>), matching the
	| BSP's crt0.c inline asm, which every m68k gas accepts.
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

| ---------------------------------------------------------------------------
| Access-error handler (vector 2). Records that a fault was taken, overwrites
| the saved PC with g_startup_resume_pc, and RTEs so the probe can continue.
| ColdFire frame: (%sp) = fmt|FS|vector|SR ; 4(%sp) = PC.
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

| ---------------------------------------------------------------------------
| Vector table, cf-isrs.c shape: entries 0/1/2 live, rest zero (NULL).
| Placed first by the linker script, so its base is the VBR programmed above.
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
