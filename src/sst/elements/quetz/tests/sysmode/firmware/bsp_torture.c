/*
 * bsp_torture.c — BSP-survival probe catalogue for QEMU mcf5208evb (m68k).
 *
 * Real ColdFire board code initializes the SoC before anything else: watchdog,
 * PLL/clocking, SCM, chip selects, GPIO, I2C. QEMU models only a few MCF5208
 * peripherals (UARTs, INTC, PITs, FEC, ...) — this firmware probes the
 * *unmodeled* BSP-init ranges and reports, per access, whether the machine
 * faults (access-error exception), reads-as-zero/writes-ignored (RAZ/WI), or
 * returns live values. It also runs the two behavioral hazards a plain
 * fault-catalogue misses:
 *
 *   - read-back-and-branch: a bounded "PLL lock bit" style poll (RAZ turns
 *     these into infinite hangs on real BSPs — worse than a fault);
 *   - write-then-verify: GPIO output readback (WI breaks it silently).
 *
 * Survives faults by installing a full 256-entry vector table (VBR moved to
 * a 1 MB-aligned SDRAM address) whose every entry lands in a handler that
 * records the ColdFire exception frame word, rewrites the saved PC to the
 * probe's resume label, and RTEs.
 *
 * This is the roadmap "BSP-survival spike" probe (plan-bsp-survival.md); the
 * output is the fault table that decides stub-device vs QEMU-overlay vs
 * porting-pattern mitigations. Always ends TESTDEV_PASS — the catalogue
 * itself is the result; assertions come with the mitigations.
 */

#include <stdint.h>

#include "coldfire_uart.h"

/* Vector table: 256 entries, at a free 1 MB-aligned SDRAM address (MCF5208
 * VBR[19:0] read as zero, so 1 MB alignment is required). Firmware + stack
 * live below 0x40100000; SDRAM in the deck is 128 MB. */
#define VEC_TABLE_ADDR  0x40800000UL

/* Globals the asm handler touches (must have external linkage). */
volatile uint32_t g_fault_flag;
volatile uint32_t g_fault_frame;   /* ColdFire frame word0: fmt|FS|vector|SR */
volatile uint32_t g_resume_pc;

/* Access-error (and everything-else) handler. ColdFire exception frame:
 *   (%sp)  = format[31:28] | FS | vector[25:18] | SR[15:0]
 *   4(%sp) = PC
 * Record the frame word, set the flag, resume the probe. ISA_A-only ops. */
__asm__(
"       .text\n"
"       .align 2\n"
"       .global bsp_fault_handler\n"
"bsp_fault_handler:\n"
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
extern void bsp_fault_handler(void);

static void install_vectors(void)
{
    volatile uint32_t *vt = (volatile uint32_t *)VEC_TABLE_ADDR;
    for (uint32_t i = 0; i < 256; i++)
        vt[i] = (uint32_t)bsp_fault_handler;
    __asm__ volatile("movec %0,%%vbr" :: "r"(VEC_TABLE_ADDR));
}

/* --- probe primitives ------------------------------------------------------
 * Each arms g_resume_pc at its own resume label (GNU computed labels), does
 * one volatile access, and reports fault state. Memory barriers keep the
 * compiler from moving anything across the faulting access. */

#define PROBE_BODY(ACCESS)                                        \
    g_fault_flag = 0;                                             \
    g_resume_pc = (uint32_t)&&resume;                             \
    __asm__ volatile("" ::: "memory");                            \
    ACCESS;                                                       \
    __asm__ volatile("" ::: "memory");                            \
resume:                                                           \
    __asm__ volatile("" ::: "memory");

static uint32_t probe_r32(uint32_t addr, uint32_t *val)
{
    uint32_t v = 0;
    PROBE_BODY(v = *(volatile uint32_t *)addr)
    *val = v;
    return g_fault_flag;
}

static uint32_t probe_r8(uint32_t addr, uint32_t *val)
{
    uint32_t v = 0;
    PROBE_BODY(v = *(volatile uint8_t *)addr)
    *val = v;
    return g_fault_flag;
}

static uint32_t probe_w32(uint32_t addr, uint32_t v)
{
    PROBE_BODY(*(volatile uint32_t *)addr = v)
    return g_fault_flag;
}

static uint32_t probe_w8(uint32_t addr, uint32_t v)
{
    PROBE_BODY(*(volatile uint8_t *)addr = (uint8_t)v)
    return g_fault_flag;
}

/* --- reporting ------------------------------------------------------------- */

static uint32_t n_fault, n_clean;

static void report(const char *name, const char *op, uint32_t addr,
                   uint32_t fault, uint32_t val)
{
    uart_puts("probe ");
    uart_puts(name);
    uart_putc(' ');
    uart_puts(op);
    uart_puts(" @");
    uart_put_u32_hex(addr);
    if (fault) {
        uart_puts(" FAULT frame=");
        uart_put_u32_hex(g_fault_frame);
        n_fault++;
    } else {
        uart_puts(" ok val=");
        uart_put_u32_hex(val);
        n_clean++;
    }
    uart_putc('\n');
}

static void probe_range(const char *name, uint32_t base)
{
    uint32_t v, f;
    f = probe_r32(base, &v);      report(name, "r32", base, f, v);
    f = probe_r8(base, &v);       report(name, "r8 ", base, f, v);
    f = probe_w32(base, 0);       report(name, "w32", base, f, 0);
}

void kernel_main(void)
{
    uart_init();
    uart_puts("BSP torture: MCF5208 init-register catalogue on mcf5208evb\n");
    install_vectors();
    uart_puts("vectors installed (VBR=40800000)\n");

    /* Controls — QEMU-modeled peripherals, expect live values, no fault. */
    uint32_t v, f;
    f = probe_r8(0xFC060004, &v);  report("UART0.SR   ", "r8 ", 0xFC060004, f, v);
    f = probe_r32(0xFC048000, &v); report("INTC0.IPRH ", "r32", 0xFC048000, f, v);
    f = probe_r32(0xFC080000, &v); report("PIT0.PCSR  ", "r32", 0xFC080000, f, v);
    f = probe_r32(0xFC030004, &v); report("FEC.EIR    ", "r32", 0xFC030004, f, v);

    /* Suspects — BSP-init ranges QEMU does not (or may not) model. */
    probe_range("SCM   ", 0xFC000000);   /* system control: MPR/PACRs */
    probe_range("XBS   ", 0xFC004000);   /* crossbar switch */
    probe_range("FBCS  ", 0xFC008000);   /* FlexBus chip selects (CSAR0) */
    probe_range("SCM2  ", 0xFC040010);   /* core watchdog CWCR area */
    probe_range("EDMA  ", 0xFC044000);
    probe_range("I2C   ", 0xFC058008);   /* I2CR */
    probe_range("QSPI  ", 0xFC05C000);
    probe_range("DTIM0 ", 0xFC070000);
    probe_range("EPORT ", 0xFC088000);
    probe_range("PLL   ", 0xFC090000);   /* clocking module PCR */
    probe_range("WTM   ", 0xFC098000);   /* watchdog WCR */
    probe_range("RCM   ", 0xFC0A0000);   /* reset controller RCR */
    probe_range("GPIO  ", 0xFC0A4000);   /* port output data regs */
    probe_range("SDRAMC", 0xFC0A8000);   /* SDRAM controller */
    probe_range("OFFMAP", 0xFC0FC000);   /* inside IPS space, no module */
    probe_range("FLASH0", 0x00000000);   /* off-SDRAM low memory */
    probe_range("WILD  ", 0xE0000000);   /* far outside the SoC map */

    /* Behavioral hazard 1: write-then-verify (GPIO output readback). */
    (void)probe_w8(0xFC0A4000, 0x5A);
    f = probe_r8(0xFC0A4000, &v);
    uart_puts("gpio write/readback: wrote 5a, read=");
    uart_put_u32_hex(v);
    uart_puts(f ? " (FAULT)" : (v == 0x5A ? " (stored)" : " (WI)"));
    uart_putc('\n');

    /* Behavioral hazard 2: bounded status poll (PLL lock style). A real BSP
     * does `while (!(PSR & LOCK));` — on RAZ this never exits. */
    uint32_t polls = 0, hit = 0;
    for (polls = 0; polls < 10000; polls++) {
        f = probe_r32(0xFC090004, &v);
        if (f) { hit = 2; break; }
        if (v != 0) { hit = 1; break; }
    }
    uart_puts("pll lock poll: ");
    uart_puts(hit == 1 ? "satisfied after " :
              hit == 2 ? "FAULT after "     : "TIMEOUT (RAZ) after ");
    uart_put_u32_dec(polls);
    uart_puts(" polls\n");

    uart_puts("catalogue done: clean=");
    uart_put_u32_dec(n_clean);
    uart_puts(" fault=");
    uart_put_u32_dec(n_fault);
    uart_putc('\n');

    /* The catalogue is the result; completion (not fault-freedom) is PASS. */
    testdev_done(TESTDEV_PASS);
}
