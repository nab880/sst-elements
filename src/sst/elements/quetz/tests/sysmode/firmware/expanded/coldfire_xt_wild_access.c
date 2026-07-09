/*
 * coldfire_xt_wild_access.c — completeness probe for finding #3
 * (review-coldfire-stack-correctness.md): a guest access outside every
 * mapped region (RAM, the MMIO bridge window, on-chip peripherals) used to
 * reach memHierarchy's MemController with an address it doesn't own, which
 * is a hard SST `fatal` -- the whole simulator aborts instead of the guest
 * merely misbehaving. The ColdFire decks now pair an explicit RAM forward
 * with the CPU's filter_unmatched_regions=1 no-match policy, so unmatched
 * addresses are counted and consumed.
 *
 * Probes four addresses in the gaps NOT covered by any handler in
 * basic_quetz_gpu_coldfire.py (RAM 0x40000000-0x47FFFFFF, MMIO window
 * 0x70000000-0x700003FF, UART 0xfc060000-0xfc0600FF, sentinel
 * 0x80000000-0x80000003): 0x08000000 (low, below RAM), 0x50000000 (between
 * RAM and the MMIO window), 0x90000000, 0xE0000000 (bsp_torture already
 * showed this last one is WILD/RAZ-WI on QEMU's side). Reuses bsp_torture's
 * fault-catching vector table so a genuine m68k bus fault is survived
 * gracefully too -- this probe is about whether the SST SIMULATOR aborts,
 * not about QEMU's own RAZ/WI behavior (already characterized by
 * test_quetz_coldfire_bsp_torture).
 *
 * The transcript is the result (no FATAL, TESTFINISH reached); the
 * testsuite additionally checks the catch-all's filtered_reads/
 * filtered_writes statistics advanced.
 *
 * SDL: sysmode/basic_quetz_gpu_coldfire.py.
 */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_fault.h"

#define VEC_TABLE_ADDR  0x40900000UL   /* distinct from bsp_torture's 0x40800000 */

static uint32_t n_fault, n_clean;

static void report(const char *name, uint32_t addr, uint32_t fault, uint32_t val)
{
    uart_puts("wild ");
    uart_puts(name);
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

static void probe_addr(const char *name, uint32_t addr)
{
    uint32_t v, f;
    f = cf_probe_r32(addr, &v);
    report(name, addr, f, v);
    f = cf_probe_w32(addr, 0x5A5A5A5Au);
    report(name, addr, f, 0);
}

void kernel_main(void)
{
    uart_init();
    uart_puts("Wild-access probe: addresses outside every region handler (m68k)\n");
    cf_install_fault_vectors(VEC_TABLE_ADDR);

    probe_addr("LOW   ", 0x08000000UL);
    probe_addr("GAP1  ", 0x50000000UL);
    probe_addr("GAP2  ", 0x90000000UL);
    probe_addr("WILD  ", 0xE0000000UL);

    uart_puts("wild probe done: clean=");
    uart_put_u32_dec(n_clean);
    uart_puts(" fault=");
    uart_put_u32_dec(n_fault);
    uart_putc('\n');

    /* Reaching here at all (rather than the SST process aborting on a
     * memHierarchy fatal) is the actual result being tested. */
    testdev_done(TESTDEV_PASS);
}
