/*
 * bsp_torture.c — BSP-survival probe catalogue for QEMU mcf5208evb (m68k).
 * Probes the *unmodeled* BSP-init ranges and records per access: fault / RAZ-WI /
 * live. Also runs the two dangerous behavioral hazards (RAZ on a lock-bit poll =
 * infinite hang, worse than a fault; WI on a GPIO readback = silent broken verify).
 * Survives faults via a full vector table that records the exception frame and
 * RTEs to a resume label. plan-bsp-survival.md; always TESTDEV_PASS (the table is
 * the result).
 */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_fault.h"   /* fault-survival scaffold + cf_probe_* */

/* Vector table: 256 entries, at a free 1 MB-aligned SDRAM address (MCF5208
 * VBR[19:0] read as zero, so 1 MB alignment is required). Firmware + stack
 * live below 0x40100000; SDRAM in the deck is 128 MB. */
#define VEC_TABLE_ADDR  0x40800000UL

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
    f = cf_probe_r32(base, &v);      report(name, "r32", base, f, v);
    f = cf_probe_r8(base, &v);       report(name, "r8 ", base, f, v);
    f = cf_probe_w32(base, 0);       report(name, "w32", base, f, 0);
}

void kernel_main(void)
{
    uart_init();
    uart_puts("BSP torture: Raptor init-register catalogue on mcf5208evb\n");
    cf_install_fault_vectors(VEC_TABLE_ADDR);
    uart_puts("vectors installed (VBR=40800000)\n");

    /* Controls — QEMU-modeled peripherals, expect live values, no fault. */
    uint32_t v, f;
    f = cf_probe_r8(0xFC060004, &v);  report("UART0.SR   ", "r8 ", 0xFC060004, f, v);
    f = cf_probe_r32(0xFC048000, &v); report("INTC0.IPRH ", "r32", 0xFC048000, f, v);
    f = cf_probe_r32(0xFC080000, &v); report("PIT0.PCSR  ", "r32", 0xFC080000, f, v);
    f = cf_probe_r32(0xFC030004, &v); report("FEC.EIR    ", "r32", 0xFC030004, f, v);

    /* Suspects — Raptor BSP-init ranges QEMU does not model. Addresses follow
     * the Raptor map (boards/raptor/board.json), not stock MCF5208: GPIOB0-3
     * occupy 0xFC084000..0xFC090000 and DTIM0-3 0xFC070000..0xFC07C000. */
    probe_range("SCM   ", 0xFC000000);   /* system control: MPR/PACRs */
    probe_range("XBS   ", 0xFC004000);   /* crossbar switch */
    probe_range("FBCS  ", 0xFC008000);   /* FlexBus chip selects (CSAR0) */
    probe_range("SCM2  ", 0xFC040010);   /* platform control / core WDT area */
    probe_range("EDMA  ", 0xFC044000);
    probe_range("I2C   ", 0xFC058008);   /* I2CR */
    probe_range("QSPI  ", 0xFC05C000);
    probe_range("DTIM0 ", 0xFC070000);
    probe_range("DTIM2 ", 0xFC078000);   /* the timer PFLASH actually uses */
    probe_range("GPIOB0", 0xFC084000);   /* Raptor GPIO bank 0 (CORE1 strap) */
    probe_range("GPIOB1", 0xFC088000);   /* Raptor GPIO bank 1 */
    probe_range("GPIOB2", 0xFC08C000);   /* Raptor GPIO bank 2 (LEDs/button) */
    probe_range("GPIOB3", 0xFC090000);   /* Raptor GPIO bank 3 (UBIO) */
    probe_range("RCM   ", 0xFC0A0000);   /* reset controller RCR */
    probe_range("LEGGPIO", 0xFC0A4000);  /* legacy MCF GPIO; unused on Raptor */
    probe_range("SDRAMC", 0xFC0A8000);   /* SDRAM controller */
    probe_range("OFFMAP", 0xFC0FC000);   /* inside IPS space, no module */
    probe_range("FLASH0", 0x00000000);   /* off-SDRAM low memory */
    probe_range("WILD  ", 0xE0000000);   /* far outside the SoC map */

    /* Behavioral hazard 1: write-then-verify against a compat-profile register.
     * GPIO now has a dedicated device (mcf-gpio) with mask-in-high-byte value
     * semantics, so it is no longer a generic storage demo; use an SCM2
     * (platform-control) register the profile marks STICKY instead. */
    (void)cf_probe_w16(0xFC040020, 0x5A);
    f = cf_probe_r16(0xFC040020, &v);
    uart_puts("compat write/readback: wrote 5a, read=");
    uart_put_u32_hex(v);
    uart_puts(f ? " (FAULT)" : (v == 0x5A ? " (stored)" : " (WI)"));
    uart_putc('\n');

    /* Width discipline: a wrong-width access to a profiled register must not
     * silently match. The profile declares SCM2+0x20 as 16-bit; a 32-bit read
     * of the same offset should fall back to RAZ and be reported loudly by the
     * compat device (the "width mismatch" diagnostic). */
    f = cf_probe_r32(0xFC040020, &v);
    uart_puts("compat width-check: r32 read=");
    uart_put_u32_hex(v);
    uart_putc('\n');

    /* Behavioral hazard 2: bounded status poll. A real BSP does
     * `while (!(STATUS & bit));` — on RAZ this never exits. Uses a profileable
     * status register in an allowlisted compat block (SCM2+0x24); the profile
     * can force it non-zero to unstick. */
    uint32_t polls = 0, hit = 0;
    for (polls = 0; polls < 10000; polls++) {
        f = cf_probe_r16(0xFC040024, &v);
        if (f) { hit = 2; break; }
        if (v != 0) { hit = 1; break; }
    }
    uart_puts("status poll: ");
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
