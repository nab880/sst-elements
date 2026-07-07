/*
 * coldfire_xt_stream_ack_early.c — completeness stress for QuetzStreamDevice's
 * data-ready IRQ re-assertion (review-coldfire-stack-correctness.md finding
 * #1b). coldfire_irq_demo.c's sensor ISR always fully drains STATUS to zero
 * before sleeping again, which never exercises "ack while data remains."
 * This firmware deliberately pops only ONE word per wake (far less than one
 * paced refill delivers) and waits SOLELY on the IRQ counter (never
 * rechecking STATUS in the sleep condition) — a legitimate driver pattern on
 * genuinely level-triggered hardware, where "more data still pending" is
 * expected to keep the line asserted and therefore keep generating wakeups.
 *
 * Before the fix (raise gated on avail_ 0->nonzero edge only), avail_ never
 * returns to exactly zero once the first refill lands ahead of consumption,
 * so no further IRQ is ever delivered after the first ack and this hangs —
 * the SST run ends on its own once the CPU and GPU have nothing left to do,
 * and the report/PASS lines below never print. After the fix (every refill
 * that delivers bytes re-asserts), each period's refill wakes the guest
 * again regardless of leftover avail_, so draining proceeds one word per
 * wake as designed.
 *
 * SDL: sysmode/basic_quetz_coldfire_system.py
 *   (QUETZ_SENSOR_IRQ_LINE=31, QUETZ_SENSOR_PACE_BYTES=32 i.e. 8 words/period
 *   -- much more than the 1 word/wake this firmware consumes).
 */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_intc.h"

#define SENSOR_BASE       0x70010000UL
#define SENSOR_STATUS     (SENSOR_BASE + 0x00UL) /* R: bytes ready now */
#define SENSOR_DATA       (SENSOR_BASE + 0x08UL) /* R: pop up to 4 bytes */
#define SENSOR_IRQ_ACK    (SENSOR_BASE + 0x28UL) /* R: raised; W1: ack */

#define IRQ_LINE_SENSOR   31
#define NEED_WAKES        4u

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}
static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static volatile uint32_t g_sensor_irqs;

__attribute__((interrupt_handler))
static void isr_sensor(void)
{
    mmio_write32(SENSOR_IRQ_ACK, 1);
    /* Wait out the INTC's stale view of the level, but only while the device
     * actually holds the line LOW (SENSOR_IRQ_ACK reads back the line
     * state): a paced refill can re-raise between the ack and the bridge's
     * next poll, and that re-assert is a genuinely new wakeup the ISR must
     * return for — spinning until IPR clears would deadlock. */
    while (cf_intc_pending(IRQ_LINE_SENSOR) && mmio_read32(SENSOR_IRQ_ACK) == 0)
        ;
    g_sensor_irqs++;
}

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire stream ack-early: pop 1 word/wake, IRQ-only wait (m68k)\n");

    cf_irq_mask_all();
    cf_vbr_init();
    cf_irq_install(IRQ_LINE_SENSOR, isr_sensor);
    cf_intc_enable(IRQ_LINE_SENSOR, 3);

    uint32_t seen = 0;
    uint32_t nonempty_after_pop = 0;

    for (uint32_t i = 0; i < NEED_WAKES; i++) {
        /* Wait purely on the IRQ counter -- no STATUS re-check in the
         * condition. On real level-triggered hardware (and the fixed
         * device) this is safe: if data remains, the line stays high and
         * a later refill re-asserts it. */
        cf_wait_until(g_sensor_irqs != seen);
        seen = g_sensor_irqs;

        uint32_t before = mmio_read32(SENSOR_STATUS);
        (void)mmio_read32(SENSOR_DATA);        /* pop exactly one word */
        uint32_t after = mmio_read32(SENSOR_STATUS);

        uart_puts("wake "); uart_put_u32_dec(i);
        uart_puts(": avail_before="); uart_put_u32_dec(before);
        uart_puts(" avail_after="); uart_put_u32_dec(after);
        uart_putc('\n');

        if (after > 0)
            nonempty_after_pop++;
    }

    uart_puts("sensor_irqs="); uart_put_u32_dec(g_sensor_irqs);
    uart_puts(" nonempty_after_pop="); uart_put_u32_dec(nonempty_after_pop);
    uart_putc('\n');

    /* Passing requires NEED_WAKES independent wakeups (proving re-assertion
     * across multiple refills, not just the initial edge) AND that most of
     * them left data behind (proving the wakeups happened while avail_ was
     * still nonzero -- the exact precondition finding #1b describes). */
    int pass = (g_sensor_irqs >= NEED_WAKES) && (nonempty_after_pop >= NEED_WAKES - 1);
    uart_puts(pass ? "STREAM ACK-EARLY PASS\n" : "STREAM ACK-EARLY FAIL\n");
    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
