/*
 * coldfire_xt_stream_ack_early.c — completeness stress for QuetzStreamDevice's
 * data-ready IRQ re-assertion (review-coldfire-stack-correctness.md finding
 * #1b). coldfire_irq_demo.c's sensor ISR always fully drains STATUS to zero
 * before sleeping again, which never exercises "ack while data remains."
 * This firmware deliberately pops only ONE word per ISR (far less than one
 * paced refill delivers). A genuinely level-triggered device must remain
 * asserted after the early ACK and re-enter until STATUS reaches zero.
 *
 * Before the true-level fix, ACK lowered the line with bytes still pending;
 * progress then depended on another paced refill. After the fix, the line
 * remains high while avail_ > 0 and falls automatically on the final pop.
 *
 * SDL: sysmode/basic_quetz_coldfire_system.py
 *   (QUETZ_SENSOR_IRQ_LINE=31, QUETZ_SENSOR_PACE_BYTES=32 i.e. 8 words/period
 *   -- much more than the 1 word/wake this firmware consumes).
 */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_intc.h"
#include "coldfire_devices.h"

#define IRQ_LINE_SENSOR   31
#define NEED_WAKES        4u

static volatile uint32_t g_sensor_irqs;
static volatile uint32_t g_before[NEED_WAKES];
static volatile uint32_t g_after[NEED_WAKES];

__attribute__((interrupt_handler))
static void isr_sensor(void)
{
    uint32_t slot = g_sensor_irqs;
    uint32_t before = mmio_read32(SENSOR_STATUS);
    if (before != 0)
        (void)mmio_read32(SENSOR_DATA);       /* exactly one word per ISR */
    uint32_t after = mmio_read32(SENSOR_STATUS);
    if (slot < NEED_WAKES) {
        g_before[slot] = before;
        g_after[slot] = after;
    }
    g_sensor_irqs = slot + 1;
    cf_isr_ack_settle(IRQ_LINE_SENSOR, SENSOR_IRQ_ACK);
}

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire stream ack-early: pop 1 word/wake, IRQ-only wait (m68k)\n");

    cf_irq_mask_all();
    cf_vbr_init();
    cf_irq_install(IRQ_LINE_SENSOR, isr_sensor);
    cf_intc_enable(IRQ_LINE_SENSOR, 3);

    cf_wait_until(g_sensor_irqs >= NEED_WAKES);

    uint32_t nonempty_after_pop = 0;
    for (uint32_t i = 0; i < NEED_WAKES; i++) {
        uart_puts("wake "); uart_put_u32_dec(i);
        uart_puts(": avail_before="); uart_put_u32_dec(g_before[i]);
        uart_puts(" avail_after="); uart_put_u32_dec(g_after[i]);
        uart_putc('\n');
        if (g_after[i] > 0)
            nonempty_after_pop++;
    }

    uart_puts("sensor_irqs="); uart_put_u32_dec(g_sensor_irqs);
    uart_puts(" nonempty_after_pop="); uart_put_u32_dec(nonempty_after_pop);
    uart_putc('\n');

    /* Most early ACKs must leave data behind while IRQ delivery continues. */
    int pass = (g_sensor_irqs >= NEED_WAKES) && (nonempty_after_pop >= NEED_WAKES - 1);
    uart_puts(pass ? "STREAM ACK-EARLY PASS\n" : "STREAM ACK-EARLY FAIL\n");
    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
