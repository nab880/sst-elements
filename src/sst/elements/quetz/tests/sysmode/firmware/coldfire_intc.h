/*
 * coldfire_intc.h — MCF5208 interrupt scaffold for QEMU mcf5208evb firmware:
 * RAM vector table + VBR, INTC0 unmask/priority, IPL mask/stop, and a
 * lost-wakeup-free wait. Documented example — see SIMULATING-YOUR-SYSTEM.md.
 * INTC0 (0xFC048000, hw/m68k/mcf_intc.c): a source fires when its ICR level != 0
 * and its IMR bit is clear (CIMR clears by source number); vector = 64 + source;
 * the IPR bit is level (stays set until the device lowers the line), so ISRs
 * spin on cf_intc_pending() after acking — guarded by the device's own line
 * state (read back via its IRQ_ACK register): if the device still holds or
 * re-raises the line (unconsumed completion events, a new paced refill), IPR
 * is legitimately set, the spin must end, and RTE re-takes the IRQ. The table
 * needs 1 MB-aligned .vectors (see link_m68k.ld).
 */

#ifndef COLDFIRE_INTC_H
#define COLDFIRE_INTC_H

#include <stdint.h>

#define INTC0_BASE   0xFC048000UL
#define INTC0_IPRH   (*(volatile uint32_t *)(INTC0_BASE + 0x00))
#define INTC0_IPRL   (*(volatile uint32_t *)(INTC0_BASE + 0x04))
#define INTC0_IMRH   (*(volatile uint32_t *)(INTC0_BASE + 0x08))
#define INTC0_IMRL   (*(volatile uint32_t *)(INTC0_BASE + 0x0C))
#define INTC0_SIMR   (*(volatile uint8_t  *)(INTC0_BASE + 0x1C))
#define INTC0_CIMR   (*(volatile uint8_t  *)(INTC0_BASE + 0x1D))
#define INTC0_ICR(n) (*(volatile uint8_t  *)(INTC0_BASE + 0x40 + (n)))

typedef void (*cf_isr_t)(void);

/* One definition per firmware image (each demo is a single .c file). Zeroed
 * entries are fine: only installed vectors are ever fetched. */
__attribute__((section(".vectors"), used, aligned(4)))
cf_isr_t cf_vector_table[256];

/* Point VBR at the table. Must run before any source is unmasked. */
static inline void cf_vbr_init(void)
{
    __asm__ volatile("movec %0,%%vbr" : : "r"(cf_vector_table) : "memory");
}

/* Install `isr` for INTC source `line` (vector 64 + line). Declare ISRs with
 * __attribute__((interrupt_handler)) so GCC saves scratch regs and RTEs. */
static inline void cf_irq_install(unsigned line, cf_isr_t isr)
{
    cf_vector_table[64u + line] = isr;
}

/* Program the source's priority level (1..6; nonzero also enables it in the
 * QEMU model) and clear its IMR mask bit. */
static inline void cf_intc_enable(unsigned line, uint8_t level)
{
    INTC0_ICR(line) = level;
    INTC0_CIMR = (uint8_t)line;
}

static inline void cf_intc_mask(unsigned line)
{
    INTC0_SIMR = (uint8_t)line;
}

static inline void cf_intc_unmask(unsigned line)
{
    INTC0_CIMR = (uint8_t)line;
}

/* 1 while INTC still sees source `line` asserted (level semantics). */
static inline uint32_t cf_intc_pending(unsigned line)
{
    if (line < 32)
        return (INTC0_IPRL >> line) & 1u;
    return (INTC0_IPRH >> (line - 32)) & 1u;
}

/* ISR epilogue for a Quetz device whose IRQ_ACK register reads back the
 * line state (GPU_IRQ_ACK, SENSOR_IRQ_ACK): ack ONE event, then wait out
 * the INTC's stale view of the level — but only while the device itself
 * reads the line low. If unconsumed completion events or a new paced refill
 * hold/re-raise the line, IPR is legitimately set: return at once and let
 * RTE re-take the IRQ; spinning until IPR clears would deadlock the ISR.
 * The guard condition is the contract with the reverse-IRQ bridge — keep it
 * here, not hand-copied per ISR. */
static inline void cf_isr_ack_settle(unsigned line, uint32_t ack_reg)
{
    *(volatile uint32_t *)ack_reg = 1;
    while (cf_intc_pending(line) && *(volatile uint32_t *)ack_reg == 0)
        ;
}

/* Re-arm a masked level source after its driver drained the condition in
 * thread context. The device reports low before the bridge necessarily has
 * propagated low to the INTC, so wait out that bounded stale-level window
 * before unmasking; otherwise the old level immediately re-enters the ISR. */
static inline void cf_intc_rearm_drained(unsigned line, uint32_t ack_reg)
{
    while (cf_intc_pending(line) && *(volatile uint32_t *)ack_reg == 0)
        ;
    cf_intc_unmask(line);
}

/* IPL 7: no interrupt below NMI is taken (supervisor mode assumed —
 * freestanding -kernel firmware never leaves it). */
static inline void cf_irq_mask_all(void)
{
    __asm__ volatile("move.w #0x2700,%%sr" : : : "memory");
}

/* Atomically drop to IPL 0 and wait for an interrupt; the ISR runs, its RTE
 * restores SR = 0x2000, and execution continues after the stop. */
static inline void cf_stop_wait(void)
{
    __asm__ volatile("stop #0x2000" : : : "memory");
}

/* Lost-wakeup-free wait: re-raise the mask BEFORE testing the condition, so
 * an interrupt cannot fire between the test and the stop — a raise in that
 * window stays pending at the INTC (level semantics) and is taken the moment
 * stop drops the IPL. Returns with interrupts masked. */
#define cf_wait_until(cond)     \
    do {                        \
        for (;;) {              \
            cf_irq_mask_all();  \
            if (cond)           \
                break;          \
            cf_stop_wait();     \
        }                       \
    } while (0)

#endif /* COLDFIRE_INTC_H */
