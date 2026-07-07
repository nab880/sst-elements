/*
 * coldfire_xt_doorbell_flood.c — general robustness stress: fire more
 * non-blocking doorbells than QuetzGpuDevice's pending-launch queue
 * (kMaxPendingLaunches=8, plus the one active op = 9 acceptable) can hold,
 * back-to-back with no waiting in between. Not tied to a specific review
 * finding -- just exploring "what happens when a guest floods the device
 * faster than it can be configured to queue." The device is documented to
 * drop excess doorbells (counted in doorbell_while_busy) rather than crash;
 * this pins that no-crash behavior for a queue-full condition under a
 * pure-latency-model (no kernel) device.
 *
 * PASS iff the accepted count settles at exactly 9 and the device never
 * fatals; the testsuite additionally checks gpu.doorbell_while_busy >= 2
 * and gpu.kernels_launched == 9.
 *
 * SDL: sysmode/basic_quetz_gpu_coldfire.py.
 */

#include <stdint.h>

#include "coldfire_uart.h"

#define GPU_BASE          0x70000000UL
#define GPU_DOORBELL      (GPU_BASE + 0x00UL)
#define GPU_STATUS        (GPU_BASE + 0x08UL)
#define GPU_KERNEL_ID     (GPU_BASE + 0x10UL)
#define GPU_LATENCY_OVR   (GPU_BASE + 0x18UL)

#define FLOOD_COUNT       11u   /* kMaxPendingLaunches(8) + 1 active + 2 excess */
#define ACCEPTED_EXPECTED 9u
/* Long enough that NO kernel retires while the flood is still being
 * submitted (each sync doorbell write costs ~1us of sim time; 200us >> 11
 * writes): a retire mid-flood pops the queue and frees a slot, making the
 * accepted count timing-dependent (10 accepted was observed with 2000). */
#define FLOOD_LATENCY     200000u

static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}
static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire doorbell flood: 11 non-blocking doorbells, no waits (m68k)\n");

    for (uint32_t i = 0; i < FLOOD_COUNT; i++) {
        mmio_write32(GPU_LATENCY_OVR, FLOOD_LATENCY);
        mmio_write32(GPU_DOORBELL, 0);
    }

    uart_puts("flood submitted: ");
    uart_put_u32_dec(FLOOD_COUNT);
    uart_putc('\n');

    uint32_t polls = 0;
    while (mmio_read32(GPU_STATUS) != 0 && polls < 1000000u)
        polls++;

    uint32_t kernel_id = mmio_read32(GPU_KERNEL_ID);
    uart_puts("settled: kernel_id=");
    uart_put_u32_dec(kernel_id);
    uart_puts(" polls=");
    uart_put_u32_dec(polls);
    uart_putc('\n');

    int pass = (kernel_id == ACCEPTED_EXPECTED) && (polls < 1000000u);
    uart_puts(pass ? "DOORBELL FLOOD PASS\n" : "DOORBELL FLOOD FAIL\n");
    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
