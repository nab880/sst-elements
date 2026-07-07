/*
 * coldfire_xt_irq_burst.c — completeness stress for QuetzGpuDevice's
 * completion-IRQ event counting (review-coldfire-stack-correctness.md
 * finding #1a). Unlike coldfire_irq_demo.c, which submits one doorbell,
 * waits for its IRQ, THEN submits the next (never letting more than one
 * completion accumulate before an ack), this fires several non-blocking
 * doorbells BACK-TO-BACK with no wait in between, so multiple kernels can
 * retire before the guest services the first interrupt.
 *
 * Phase 1: fire BATCH1 doorbells back-to-back with staggered latencies; the
 * ISR acks exactly one completion event per interrupt taken (mirrors a
 * one-completion-per-IRQ driver). PASS iff the ISR-counted completions
 * reach BATCH1 (pre-fix, only the first completion's IRQ was ever
 * observable and this would hang until the SST run gives up and ends the
 * simulation with the report/PASS lines never printed).
 *
 * Phase 2: fire BATCH2 more doorbells back-to-back with IRQs masked, busy-
 * poll REG_KERNEL_ID (which itself drives retirement) until all BATCH2 have
 * retired internally, then ack with a wildcard (~0) value in one write and
 * confirm REG_IRQ_ACK reads 0 immediately — proving "ack value = events to
 * consume, ~0 acks everything" in one shot.
 *
 * SDL: sysmode/basic_quetz_coldfire_system.py (QUETZ_GPU_IRQ_LINE=30); the
 * sensor device is present but untouched by this firmware.
 */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_intc.h"

#define GPU_BASE          0x70000000UL
#define GPU_DOORBELL      (GPU_BASE + 0x00UL)   /* W: submit (non-blocking) */
#define GPU_STATUS        (GPU_BASE + 0x08UL)   /* R: busy(1)/idle(0) */
#define GPU_KERNEL_ID     (GPU_BASE + 0x10UL)   /* R: completed counter */
#define GPU_LATENCY_OVR   (GPU_BASE + 0x18UL)   /* W: next-kernel cycles */
#define GPU_IRQ_ACK       (GPU_BASE + 0x50UL)   /* R: raised; W: consume N */

#define IRQ_LINE_ACCEL    30

#define BATCH1            5u
#define BATCH2            3u

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
    /* Wait out the INTC's stale view of the level, but only while the device
     * actually holds the line LOW (GPU_IRQ_ACK reads back the line state).
     * With event counting, unconsumed completions keep the line raised on
     * purpose — the IRQ must simply be re-taken after RTE; spinning until
     * IPR clears would deadlock the ISR. */
    while (cf_intc_pending(IRQ_LINE_ACCEL) && mmio_read32(GPU_IRQ_ACK) == 0)
        ;
    g_accel_irqs++;
}

static void submit(uint32_t latency)
{
    mmio_write32(GPU_LATENCY_OVR, latency);
    mmio_write32(GPU_DOORBELL, 0);      /* non-blocking */
}

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire IRQ burst: multi-op completions before ack (m68k)\n");

    cf_irq_mask_all();
    cf_vbr_init();
    cf_irq_install(IRQ_LINE_ACCEL, isr_accel);
    cf_intc_enable(IRQ_LINE_ACCEL, 3);

    /* --- Phase 1: interrupt-driven, one ack per event --------------------- */
    for (uint32_t i = 0; i < BATCH1; i++)
        submit(3000u * (i + 1));        /* staggered so retires don't coincide */

    cf_wait_until(g_accel_irqs >= BATCH1);

    uint32_t kid_after_p1 = mmio_read32(GPU_KERNEL_ID);
    uart_puts("phase1: accel_irqs=");
    uart_put_u32_dec(g_accel_irqs);
    uart_puts(" kernel_id=");
    uart_put_u32_dec(kid_after_p1);
    uart_putc('\n');

    /* --- Phase 2: mask IRQs, poll to retire, ack everything at once ------- */
    cf_irq_mask_all();
    for (uint32_t i = 0; i < BATCH2; i++)
        submit(2500u * (i + 1));

    uint32_t target = kid_after_p1 + BATCH2;
    uint32_t polls = 0;
    while (mmio_read32(GPU_KERNEL_ID) < target && polls < 2000000u)
        polls++;
    uint32_t kid_after_p2 = mmio_read32(GPU_KERNEL_ID);

    mmio_write32(GPU_IRQ_ACK, 0xFFFFFFFFu);   /* wildcard: consume everything */
    uint32_t ack_after_wildcard = mmio_read32(GPU_IRQ_ACK);

    uart_puts("phase2: kernel_id=");
    uart_put_u32_dec(kid_after_p2);
    uart_puts(" polls=");
    uart_put_u32_dec(polls);
    uart_puts(" irq_ack_after_wildcard=");
    uart_put_u32_dec(ack_after_wildcard);
    uart_putc('\n');

    int pass = (g_accel_irqs == BATCH1) && (kid_after_p1 == BATCH1)
            && (kid_after_p2 == target) && (ack_after_wildcard == 0);
    uart_puts(pass ? "IRQ BURST PASS\n" : "IRQ BURST FAIL\n");
    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
