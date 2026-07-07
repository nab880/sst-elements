/*
 * coldfire_xt_irq_zero_latency.c — completion IRQ for ZERO-latency kernels.
 * startKernel()'s latency==0 shortcut used to complete the op (kernel_id++)
 * without ever calling raiseIrqOnRetire(), so an ISR-driven guest that
 * submits with LATENCY_OVR=0 waited forever. This fires BATCH zero-latency
 * doorbells (each completes instantly, the device is never BUSY between
 * them) and requires one counted IRQ per completion; pre-fix, the first
 * cf_wait_until never returns and the report/PASS lines never print.
 *
 * SDL: sysmode/basic_quetz_coldfire_system.py (QUETZ_GPU_IRQ_LINE=30); the
 * sensor device is present but untouched by this firmware.
 */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_intc.h"

#define GPU_BASE          0x70000000UL
#define GPU_DOORBELL      (GPU_BASE + 0x00UL)   /* W: submit (non-blocking) */
#define GPU_KERNEL_ID     (GPU_BASE + 0x10UL)   /* R: completed counter */
#define GPU_LATENCY_OVR   (GPU_BASE + 0x18UL)   /* W: next-kernel cycles */
#define GPU_IRQ_ACK       (GPU_BASE + 0x50UL)   /* R: raised; W: consume N */

#define IRQ_LINE_ACCEL    30

#define BATCH             3u

static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static volatile uint32_t g_accel_irqs;

__attribute__((interrupt_handler))
static void isr_accel(void)
{
    mmio_write32(GPU_IRQ_ACK, 1);       /* consume exactly one event */
    /* Guarded stale-level spin (see coldfire_intc.h): if unconsumed events
     * hold the line, return and let RTE re-take the IRQ. */
    while (cf_intc_pending(IRQ_LINE_ACCEL) && mmio_read32(GPU_IRQ_ACK) == 0)
        ;
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

    int pass = (g_accel_irqs == BATCH) && (kid == BATCH);
    uart_puts(pass ? "ZERO LATENCY IRQ PASS\n" : "ZERO LATENCY IRQ FAIL\n");
    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
