/*
 * coldfire_irq_demo.c — ISR-driven completion against SST-window devices,
 * ColdFire m68k. Accel (QuetzGpuDevice irq_line) and paced sensors
 * (QuetzStreamDevice irq_line) interrupt the guest instead of being polled; the
 * ISRs ack the device then spin until the INTC IPR bit drops (the line lowers a
 * bridge poll tick after ack) so RTE can't re-take a stale level. PASS iff both
 * accel IRQs and >=1 sensor IRQ arrive and the guest actually slept.
 * SDL: sysmode/basic_quetz_coldfire_system.py (GPU_IRQ_LINE=30, SENSOR=31).
 */

#include <stdint.h>

#include "coldfire_uart.h"
#include "coldfire_intc.h"

#define GPU_BASE          0x70000000UL
#define GPU_DOORBELL      (GPU_BASE + 0x00UL)   /* W: submit */
#define GPU_STATUS        (GPU_BASE + 0x08UL)   /* R: busy(1)/idle(0) */
#define GPU_KERNEL_ID     (GPU_BASE + 0x10UL)   /* R: completed counter */
#define GPU_LATENCY_OVR   (GPU_BASE + 0x18UL)   /* W: next-kernel cycles */
#define GPU_IRQ_ACK       (GPU_BASE + 0x50UL)   /* R: raised; W1: ack */

#define SENSOR_BASE       0x70010000UL
#define SENSOR_STATUS     (SENSOR_BASE + 0x00UL) /* R: bytes ready now */
#define SENSOR_DATA       (SENSOR_BASE + 0x08UL) /* R: pop 4 bytes (packed) */
#define SENSOR_SEQ        (SENSOR_BASE + 0x10UL) /* R: bytes consumed */
#define SENSOR_EOS        (SENSOR_BASE + 0x20UL) /* R: fully consumed */
#define SENSOR_IRQ_ACK    (SENSOR_BASE + 0x28UL) /* R: raised; W1: ack */

/* Machine INTC source numbers — must match the deck's QUETZ_GPU_IRQ_LINE /
 * QUETZ_SENSOR_IRQ_LINE. 30/31 are unused on QEMU's mcf5208evb (UARTs are
 * 26-28, PITs 4-5, FEC 36+). */
#define IRQ_LINE_ACCEL    30
#define IRQ_LINE_SENSOR   31

static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* --- ISRs -------------------------------------------------------------------
 * Ack the device, then wait for the INTC to see the line drop (the lower
 * propagates via the bridge's poll timer, ~10 us of virtual time) before
 * returning — otherwise RTE would re-enter on the stale level. Counters are
 * the ISR -> main handshake. */

static volatile uint32_t g_accel_irqs;
static volatile uint32_t g_sensor_irqs;

__attribute__((interrupt_handler))
static void isr_accel(void)
{
    mmio_write32(GPU_IRQ_ACK, 1);
    while (cf_intc_pending(IRQ_LINE_ACCEL))
        ;
    g_accel_irqs++;
}

__attribute__((interrupt_handler))
static void isr_sensor(void)
{
    mmio_write32(SENSOR_IRQ_ACK, 1);
    while (cf_intc_pending(IRQ_LINE_SENSOR))
        ;
    g_sensor_irqs++;
}

/* --- Accelerator: ISR-driven completion ------------------------------------ */

static uint32_t g_accel_sleeps;

static int accel_irq_batches(uint32_t batches)
{
    for (uint32_t i = 0; i < batches; i++) {
        mmio_write32(GPU_LATENCY_OVR, 20000 * (i + 1));
        mmio_write32(GPU_DOORBELL, 0);           /* non-blocking submit */
        if (g_accel_irqs < i + 1)
            g_accel_sleeps++;
        cf_wait_until(g_accel_irqs >= i + 1);    /* stop, no STATUS polling */
    }
    if (mmio_read32(GPU_KERNEL_ID) != batches)
        return 0;
    if (mmio_read32(GPU_STATUS) != 0)
        return 0;
    if (mmio_read32(GPU_IRQ_ACK) != 0)           /* line acked + lowered */
        return 0;
    return 1;
}

/* --- Sensors: data-ready-IRQ-driven drain ----------------------------------
 * Same stream + sum32-trailer verification as coldfire_system.c, but the
 * "not ready yet" branch sleeps until the next data-ready IRQ instead of
 * spinning on STATUS. */

static uint32_t g_sensor_sleeps;

static int sensor_drain_irq(void)
{
    uint32_t sum = 0, prev = 0, prev_valid = 0;

    for (;;) {
        if (mmio_read32(SENSOR_STATUS) == 0) {
            if (mmio_read32(SENSOR_EOS))
                break;                           /* fully drained */
            uint32_t seen = g_sensor_irqs;
            g_sensor_sleeps++;
            cf_wait_until(g_sensor_irqs != seen ||
                          mmio_read32(SENSOR_STATUS) != 0);
            continue;
        }
        uint32_t w = mmio_read32(SENSOR_DATA);
        if (prev_valid) {
            sum += (prev & 0xFFu) + ((prev >> 8) & 0xFFu)
                 + ((prev >> 16) & 0xFFu) + ((prev >> 24) & 0xFFu);
        }
        prev = w;
        prev_valid = 1;
    }

    uint32_t total = mmio_read32(SENSOR_SEQ);
    if (total < 8 || (total & 3u))
        return 0;
    return prev_valid && sum == prev;            /* last word = trailer */
}

/* --------------------------------------------------------------------------- */

void kernel_main(void)
{
    uart_init();
    uart_puts("ColdFire IRQ demo: ISR-driven accel + sensor stream (m68k)\n");

    cf_irq_mask_all();
    cf_vbr_init();
    cf_irq_install(IRQ_LINE_ACCEL, isr_accel);
    cf_irq_install(IRQ_LINE_SENSOR, isr_sensor);
    cf_intc_enable(IRQ_LINE_ACCEL, 3);
    cf_intc_enable(IRQ_LINE_SENSOR, 3);

    /* Sensors first: the paced refills (4 x 100us) arrive while this phase
     * runs, so the drain provably sleeps between them. (Run second, the
     * accel phase's several ms would let every refill land first and the
     * drain would never wait.) */
    int sensor_ok = sensor_drain_irq();
    uart_puts("sensors: stream=");
    uart_puts(sensor_ok ? "ok" : "BAD");
    uart_puts(" irqs=");
    uart_put_u32_dec(g_sensor_irqs);
    uart_puts(" sleeps=");
    uart_put_u32_dec(g_sensor_sleeps);
    uart_putc('\n');

    int accel_ok = accel_irq_batches(2);
    uart_puts("accel: irqs=");
    uart_put_u32_dec(g_accel_irqs);
    uart_puts(" kernels=");
    uart_put_u32_dec(mmio_read32(GPU_KERNEL_ID));
    uart_puts(" sleeps=");
    uart_put_u32_dec(g_accel_sleeps);
    uart_putc('\n');

    int pass = accel_ok && sensor_ok &&
               g_accel_irqs == 2 && g_sensor_irqs > 0 &&
               g_accel_sleeps > 0 && g_sensor_sleeps > 0;
    uart_puts(pass ? "IRQ DEMO PASS\n" : "IRQ DEMO FAIL\n");

    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
