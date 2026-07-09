/*
 * coldfire_xt_irq_zero_latency.c — completion IRQ for ZERO-latency kernels.
 *
 * Phase 1: startKernel()'s latency==0 shortcut used to complete the op
 * (kernel_id++) without ever calling raiseIrqOnRetire(), so an ISR-driven
 * guest that submits with LATENCY_OVR=0 waited forever. Fires BATCH
 * zero-latency doorbells (each completes instantly, the device is never
 * BUSY between them) and requires one counted IRQ per completion; pre-fix,
 * the first cf_wait_until never returns.
 *
 * Phase 2: zero-latency doorbells QUEUED behind a busy op. retireIfReady()
 * used to pop only ONE pending doorbell per retire; a zero-latency pop
 * completes instantly and leaves the device un-busy, so nothing ever popped
 * the doorbells queued behind it — the queue wedged forever (kernel_id
 * never advances, the device holds the sim open until the harness timeout).
 * Submits one slow op, queues QBATCH zero-latency doorbells behind it while
 * it is busy, and requires every completion's IRQ.
 *
 * SDL: sysmode/basic_quetz_coldfire_system.py (QUETZ_GPU_IRQ_LINE=30); the
 * sensor device is present but untouched by this firmware.
 */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_intc.h"
#include "coldfire_devices.h"

#define IRQ_LINE_ACCEL    30

#define BATCH             3u
#define QBATCH            2u    /* zero-latency doorbells queued behind a busy op */
#define SLOW_CYCLES       20000u

static volatile uint32_t g_accel_irqs;

__attribute__((interrupt_handler))
static void isr_accel(void)
{
    /* Ack exactly one completion event, guarded settle (coldfire_intc.h). */
    cf_isr_ack_settle(IRQ_LINE_ACCEL, GPU_IRQ_ACK);
    g_accel_irqs++;
}

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire IRQ zero-latency: instant completions must interrupt (m68k)\n");

    cf_irq_mask_all();
    cf_vbr_init();
    cf_irq_install(IRQ_LINE_ACCEL, isr_accel);
    cf_intc_enable(IRQ_LINE_ACCEL, 3);

    for (uint32_t i = 0; i < BATCH; i++) {
        mmio_write32(GPU_LATENCY_OVR, 0);   /* zero latency: retire at doorbell */
        mmio_write32(GPU_DOORBELL, 0);      /* non-blocking */
        cf_wait_until(g_accel_irqs >= i + 1);
    }

    uint32_t kid = mmio_read32(GPU_KERNEL_ID);
    uart_puts("zero-latency: accel_irqs=");
    uart_put_u32_dec(g_accel_irqs);
    uart_puts(" kernel_id=");
    uart_put_u32_dec(kid);
    uart_putc('\n');

    /* --- Phase 2: zero-latency doorbells queued behind a busy op ----------
     * The slow op keeps the device BUSY long enough (20000 cycles >> the
     * few sync-MMIO round trips below) that both zero-latency doorbells are
     * queued, not started directly. When the slow op retires, the device
     * must drain BOTH from the queue (pre-fix it popped one and wedged). */
    mmio_write32(GPU_LATENCY_OVR, SLOW_CYCLES);
    mmio_write32(GPU_DOORBELL, 0);
    for (uint32_t i = 0; i < QBATCH; i++) {
        mmio_write32(GPU_LATENCY_OVR, 0);
        mmio_write32(GPU_DOORBELL, 0);
    }
    uint32_t target = BATCH + 1u + QBATCH;
    cf_wait_until(g_accel_irqs >= target);

    uint32_t kid2 = mmio_read32(GPU_KERNEL_ID);
    uart_puts("queued-zero-latency: accel_irqs=");
    uart_put_u32_dec(g_accel_irqs);
    uart_puts(" kernel_id=");
    uart_put_u32_dec(kid2);
    uart_putc('\n');

    int pass = (g_accel_irqs == target) && (kid == BATCH) && (kid2 == target);
    uart_puts(pass ? "ZERO LATENCY IRQ PASS\n" : "ZERO LATENCY IRQ FAIL\n");
    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
