/*
 * coldfire_fault.h — m68k fault-survival scaffold: a full RAM vector table
 * whose every entry records the ColdFire exception frame and RTEs to a
 * pre-armed resume label, so a probe can take a genuine bus fault and keep
 * going. Single source for bsp_torture.c and the expanded wild-access probe
 * (each firmware image is a single .c, so including this instantiates one
 * private copy per binary).
 *
 * ColdFire exception frame:
 *   (%sp)  = format[31:28] | FS | vector[25:18] | SR[15:0]
 *   4(%sp) = PC
 * The handler records the frame word, sets the flag, overwrites the saved PC
 * with g_resume_pc, and RTEs. ISA_A-only ops.
 */

#ifndef COLDFIRE_FAULT_H
#define COLDFIRE_FAULT_H

#include <stdint.h>

/* Globals the asm handler touches (must have external linkage). */
volatile uint32_t g_fault_flag;
volatile uint32_t g_fault_frame;   /* ColdFire frame word0: fmt|FS|vector|SR */
volatile uint32_t g_resume_pc;

__asm__(
"       .text\n"
"       .align 2\n"
"       .global cf_fault_handler\n"
"cf_fault_handler:\n"
"       move.l  %a0,-(%sp)\n"
"       move.l  %d0,-(%sp)\n"
"       moveq   #1,%d0\n"
"       lea     g_fault_flag,%a0\n"
"       move.l  %d0,(%a0)\n"
"       move.l  8(%sp),%d0\n"          /* frame word0 (past 2 saves) */
"       lea     g_fault_frame,%a0\n"
"       move.l  %d0,(%a0)\n"
"       lea     g_resume_pc,%a0\n"
"       move.l  (%a0),%d0\n"
"       move.l  %d0,12(%sp)\n"         /* overwrite saved PC */
"       move.l  (%sp)+,%d0\n"
"       move.l  (%sp)+,%a0\n"
"       rte\n"
);
extern void cf_fault_handler(void);

/* Fill a 256-entry vector table at `vec_table_addr` with the fault handler
 * and point VBR at it. MCF5208 VBR[19:0] read as zero, so the address must
 * be 1 MB aligned and in otherwise-unused SDRAM. */
static void cf_install_fault_vectors(uint32_t vec_table_addr)
{
    volatile uint32_t *vt = (volatile uint32_t *)vec_table_addr;
    for (uint32_t i = 0; i < 256; i++)
        vt[i] = (uint32_t)cf_fault_handler;
    __asm__ volatile("movec %0,%%vbr" :: "r"(vec_table_addr));
}

/* Each probe arms g_resume_pc at its own resume label (GNU computed labels),
 * does one volatile access, and reports fault state. Memory barriers keep
 * the compiler from moving anything across the faulting access. */
#define CF_PROBE_BODY(ACCESS)                                     \
    g_fault_flag = 0;                                             \
    g_resume_pc = (uint32_t)&&resume;                             \
    __asm__ volatile("" ::: "memory");                            \
    ACCESS;                                                       \
    __asm__ volatile("" ::: "memory");                            \
resume:                                                           \
    __asm__ volatile("" ::: "memory");

__attribute__((unused))
static uint32_t cf_probe_r32(uint32_t addr, uint32_t *val)
{
    uint32_t v = 0;
    CF_PROBE_BODY(v = *(volatile uint32_t *)addr)
    *val = v;
    return g_fault_flag;
}

__attribute__((unused))
static uint32_t cf_probe_r8(uint32_t addr, uint32_t *val)
{
    uint32_t v = 0;
    CF_PROBE_BODY(v = *(volatile uint8_t *)addr)
    *val = v;
    return g_fault_flag;
}

__attribute__((unused))
static uint32_t cf_probe_r16(uint32_t addr, uint32_t *val)
{
    uint32_t v = 0;
    CF_PROBE_BODY(v = *(volatile uint16_t *)addr)
    *val = v;
    return g_fault_flag;
}

__attribute__((unused))
static uint32_t cf_probe_w32(uint32_t addr, uint32_t v)
{
    CF_PROBE_BODY(*(volatile uint32_t *)addr = v)
    return g_fault_flag;
}

__attribute__((unused))
static uint32_t cf_probe_w8(uint32_t addr, uint32_t v)
{
    CF_PROBE_BODY(*(volatile uint8_t *)addr = (uint8_t)v)
    return g_fault_flag;
}

__attribute__((unused))
static uint32_t cf_probe_w16(uint32_t addr, uint32_t v)
{
    CF_PROBE_BODY(*(volatile uint16_t *)addr = (uint16_t)v)
    return g_fault_flag;
}

#endif /* COLDFIRE_FAULT_H */
