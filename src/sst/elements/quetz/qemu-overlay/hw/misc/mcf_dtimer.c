/*
 * mcf_dtimer.c -- Raptor ColdFire V4 DMA Timer (DTIM0-3) model.
 *
 * A Path A QEMU SysBus-free device (mapped like mcf_bsp_compat) modeling the
 * four DMA Timer modules the Raptor BSP uses. Unlike mcf-bsp-compat, which is
 * register storage only, this device advances the counter register (DTCN) with
 * QEMU virtual time so that BSP busy-wait sleeps terminate and the dtimer
 * tester observes real, per-module-independent counts.
 *
 * Contract (raptor-bsp src/raptor_drivers/dtimer/DTIMER_driver.{c,h},
 * blockers.md 3):
 *
 *   base 0xFC070000, four modules at 0x4000 spacing (DTIM0..3).
 *
 *   off  reg    width  reset        access
 *   +00  DTMR   16     0x0000       R/W   mode (PS CE OM ORRI FRR CLK RST)
 *   +02  DTXMR  8      0x00         R/W   ext mode (DMAEN HALTED MODE16)
 *   +03  DTER   8      0x00         R, write-1-to-clear (CAP=b0 REF=b1)
 *   +04  DTRR   32     0xFFFFFFFF   R/W   reference
 *   +08  DTCR   32     0x00000000   R     capture (never latched here)
 *   +0C  DTCN   32     0x00000000   R, but the driver WRITES 0 to clear it
 *
 * Counter model:
 *   - Runs iff RST=1 (DTMR bit0) and CLK != 0 (DTMR bits 2:1).
 *   - Rate: CLK=1 -> module clock (SLOW_CLOCK_RATE = 62.5 MHz);
 *           CLK=2 -> module clock / 16; CLK=3 (external timer_in) -> treated
 *           as the module clock (no external stimulus modeled).
 *   - MODE16 (DTXMR bit0) multiplies the increment by 65537 (diagnostic mode).
 *   - A write to DTCN (any value) clears the counter and re-bases it, matching
 *     DTIMER_reset_timer_counter(). The header calls DTCN read-only, but stock
 *     BSP code depends on this clearing write; honoring it is mandatory.
 *
 * Timing is functional, not cycle-accurate: assert behavior, not exact counts.
 * No interrupt/DMA path is modeled (ORRI/DMAEN are stored but inert) -- the BSP
 * and every tester poll DTCN; none install a timer vector (blockers.md 1, 3).
 */

#include "qemu/osdep.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#include "raptor_dtimer_blocks.h"

#define TYPE_MCF_DTIMER "mcf-dtimer"
OBJECT_DECLARE_SIMPLE_TYPE(McfDtimerState, MCF_DTIMER)

/* SLOW_CLOCK_RATE from raptor_board_settings.h: the module clock feeding the
 * prescaler when CLK=1. Pinned by the BSP; see blockers.md 3 "Timing". */
#define DTIMER_MODULE_CLOCK_HZ 62500000ULL

/* Register byte offsets within a module. */
#define DTMR_OFFSET   0x00
#define DTXMR_OFFSET  0x02
#define DTER_OFFSET   0x03
#define DTRR_OFFSET   0x04
#define DTCR_OFFSET   0x08
#define DTCN_OFFSET   0x0C

/* DTMR bit fields. */
#define DTMR_RST_BIT  0x0001            /* bit 0: 1 = enabled, 0 = in reset */
#define DTMR_CLK_MASK 0x0006            /* bits 2:1 clock select */
#define DTMR_CLK_SHIFT 1

/* DTXMR bit fields. */
#define DTXMR_MODE16  0x01              /* bit 0: increment by 65537 */

/* DTER event bits (write-1-to-clear). */
#define DTER_EVENT_MASK 0x03

typedef struct McfDtimerModule {
    struct McfDtimerState *owner;
    char *name;
    uint64_t base;
    uint64_t size;
    MemoryRegion region;
    bool mapped;

    /* Programmed register state. */
    uint16_t dtmr;
    uint8_t  dtxmr;
    uint8_t  dter;
    uint32_t dtrr;
    uint32_t dtcr;

    /* Counter model. accum holds counts banked while stopped or at the last
     * re-base; base_ns is the virtual-time origin for the currently running
     * span; running mirrors (RST=1 && CLK!=0) as of the last register change. */
    uint64_t accum;
    int64_t  base_ns;
    bool     running;
    QEMUTimer *heartbeat;
} McfDtimerModule;

struct McfDtimerState {
    DeviceState parent_obj;
    char *target;
    bool strict_mmio;
    McfDtimerModule modules[G_N_ELEMENTS(raptor_dtimer_blocks)];
};

static void dtimer_bad_access(McfDtimerModule *m, const char *op,
                              hwaddr offset, unsigned size)
{
    if (m->owner->strict_mmio) {
        error_report("RAPTOR_MMIO_UNSUPPORTED owner=%s op=%s "
                     "addr=0x%08" PRIx64 " size=%u",
                     m->name, op, m->base + offset, size);
        exit(EXIT_FAILURE);
    }
    warn_report("mcf-dtimer: %s: unmodeled/wrong-width %s at +0x%" PRIx64
                " (size %u); access remains RAZ/WI",
                m->name, op, (uint64_t)offset, size);
}

static bool dtmr_should_run(uint16_t dtmr)
{
    return (dtmr & DTMR_RST_BIT) &&
           ((dtmr & DTMR_CLK_MASK) >> DTMR_CLK_SHIFT) != 0;
}

static void dtimer_heartbeat(void *opaque)
{
    McfDtimerModule *m = opaque;

    if (m->running) {
        timer_mod(m->heartbeat,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000LL);
    }
}

/* Counts elapsed over ns nanoseconds at the module's current clock select.
 * Uses 64-bit math with round-to-nearest so long spans do not drift. */
static uint64_t dtimer_counts_for_ns(const McfDtimerModule *m, uint64_t ns)
{
    uint64_t hz = DTIMER_MODULE_CLOCK_HZ;
    unsigned clk = (m->dtmr & DTMR_CLK_MASK) >> DTMR_CLK_SHIFT;
    uint64_t counts;

    if (clk == 2) {
        hz /= 16;                       /* module clock / 16 */
    }
    /* clk == 1 or 3: module clock (external timer_in unmodeled -> module clk). */

    /* counts = round(ns * hz / 1e9), computed to avoid overflow for the ns
     * ranges the BSP uses (seconds -> ~1e9 ns; hz <= 6.25e7). */
    counts = (ns / 1000000000ULL) * hz;
    counts += ((ns % 1000000000ULL) * hz + 500000000ULL) / 1000000000ULL;

    if (m->dtxmr & DTXMR_MODE16) {
        counts *= 65537ULL;             /* diagnostic high-count mode */
    }
    return counts;
}

/* Current DTCN value: banked counts plus whatever has elapsed while running. */
static uint32_t dtimer_current_count(const McfDtimerModule *m)
{
    uint64_t total = m->accum;

    if (m->running) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (now > m->base_ns) {
            total += dtimer_counts_for_ns(m, (uint64_t)(now - m->base_ns));
        }
    }
    return (uint32_t)total;             /* 32-bit counter wraps naturally */
}

/* Fold any elapsed running time into accum and drop out of the running span.
 * Call before changing clock/enable state or on a counter reset. */
static void dtimer_freeze(McfDtimerModule *m)
{
    if (m->running) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (now > m->base_ns) {
            m->accum += dtimer_counts_for_ns(m, (uint64_t)(now - m->base_ns));
        }
        m->running = false;
        timer_del(m->heartbeat);
    }
}

/* Recompute run state from the current DTMR/DTXMR, re-basing the running span
 * origin on a stopped->running transition. accum already reflects prior spans. */
static void dtimer_resync(McfDtimerModule *m)
{
    bool want = dtmr_should_run(m->dtmr);

    if (want && !m->running) {
        m->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        m->running = true;
        /* Keep a virtual-clock deadline pending while the counter runs. This
         * lets qtest advance time even though DTCN is calculated lazily. */
        timer_mod(m->heartbeat, m->base_ns + 1000000000LL);
    } else if (!want && m->running) {
        dtimer_freeze(m);
    }
}

static uint64_t dtimer_read(void *opaque, hwaddr offset, unsigned size)
{
    McfDtimerModule *m = opaque;

    switch (offset) {
    case DTMR_OFFSET:
        if (size == 2) {
            return m->dtmr;
        }
        break;
    case DTXMR_OFFSET:
        if (size == 1) {
            return m->dtxmr;
        }
        break;
    case DTER_OFFSET:
        if (size == 1) {
            return m->dter & DTER_EVENT_MASK;
        }
        break;
    case DTRR_OFFSET:
        if (size == 4) {
            return m->dtrr;
        }
        break;
    case DTCR_OFFSET:
        if (size == 4) {
            return m->dtcr;
        }
        break;
    case DTCN_OFFSET:
        if (size == 4) {
            return dtimer_current_count(m);
        }
        break;
    default:
        break;
    }
    dtimer_bad_access(m, "read", offset, size);
    return 0;
}

static void dtimer_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    McfDtimerModule *m = opaque;

    switch (offset) {
    case DTMR_OFFSET:
        if (size == 2) {
            /* Bank elapsed counts at the old clock before adopting a new
             * CLK value. Otherwise a live clock change retroactively re-rates
             * the entire running span. */
            dtimer_freeze(m);
            m->dtmr = (uint16_t)value;
            dtimer_resync(m);
            return;
        }
        break;
    case DTXMR_OFFSET:
        if (size == 1) {
            /* MODE16 scales the increment; fold elapsed counts at the old
             * scale before adopting the new one so the change is not
             * retroactive. */
            dtimer_freeze(m);
            m->dtxmr = (uint8_t)value;
            dtimer_resync(m);
            return;
        }
        break;
    case DTER_OFFSET:
        if (size == 1) {
            /* Write-1-to-clear the event flags. */
            m->dter &= ~((uint8_t)value & DTER_EVENT_MASK);
            return;
        }
        break;
    case DTRR_OFFSET:
        if (size == 4) {
            m->dtrr = (uint32_t)value;
            return;
        }
        break;
    case DTCN_OFFSET:
        if (size == 4) {
            /* DTIMER_reset_timer_counter(): clear the counter and re-base the
             * running span. The header marks DTCN read-only, but stock BSP
             * sleeps depend on this clearing write (blockers.md 3). */
            m->accum = 0;
            m->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            return;
        }
        break;
    case DTCR_OFFSET:
        /* Capture register is read-only; a functional sim never latches it. */
        dtimer_bad_access(m, "write-read-only", offset, size);
        return;
    default:
        break;
    }
    dtimer_bad_access(m, "write", offset, size);
}

static const MemoryRegionOps dtimer_ops = {
    .read = dtimer_read,
    .write = dtimer_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl  = { .min_access_size = 1, .max_access_size = 4 },
};

static void dtimer_module_reset(McfDtimerModule *m)
{
    if (m->heartbeat) {
        timer_del(m->heartbeat);
    }
    m->dtmr = 0x0000;
    m->dtxmr = 0x00;
    m->dter = 0x00;
    m->dtrr = 0xFFFFFFFF;               /* reset value per the contract */
    m->dtcr = 0x00000000;
    m->accum = 0;
    m->base_ns = 0;
    m->running = false;
}

static void mcf_dtimer_realize(DeviceState *dev, Error **errp)
{
    McfDtimerState *s = MCF_DTIMER(dev);
    size_t i;

    if (!s->target || strcmp(s->target, RAPTOR_DTIMER_TARGET)) {
        error_setg(errp, "mcf-dtimer: only target="
                   RAPTOR_DTIMER_TARGET " is supported");
        return;
    }

    for (i = 0; i < G_N_ELEMENTS(raptor_dtimer_blocks); i++) {
        const RaptorDtimerBlockDesc *d = &raptor_dtimer_blocks[i];
        McfDtimerModule *m = &s->modules[i];

        m->owner = s;
        m->name = g_strdup(d->name);
        m->base = d->base;
        m->size = d->size;
        m->heartbeat = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    dtimer_heartbeat, m);
        dtimer_module_reset(m);

        memory_region_init_io(&m->region, OBJECT(dev), &dtimer_ops, m,
                              m->name, m->size);
        memory_region_add_subregion_overlap(get_system_memory(), m->base,
                                            &m->region, 1);
        m->mapped = true;
    }
}

static void mcf_dtimer_unrealize(DeviceState *dev)
{
    McfDtimerState *s = MCF_DTIMER(dev);
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(s->modules); i++) {
        McfDtimerModule *m = &s->modules[i];
        if (m->mapped) {
            memory_region_del_subregion(get_system_memory(), &m->region);
            object_unparent(OBJECT(&m->region));
            m->mapped = false;
        }
        timer_free(m->heartbeat);
        m->heartbeat = NULL;
        g_free(m->name);
        m->name = NULL;
    }
}

static void mcf_dtimer_reset(DeviceState *dev)
{
    McfDtimerState *s = MCF_DTIMER(dev);
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(s->modules); i++) {
        dtimer_module_reset(&s->modules[i]);
    }
}

static Property mcf_dtimer_properties[] = {
    DEFINE_PROP_STRING("target", McfDtimerState, target),
    DEFINE_PROP_BOOL("strict-mmio", McfDtimerState, strict_mmio, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void mcf_dtimer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcf_dtimer_realize;
    dc->unrealize = mcf_dtimer_unrealize;
    device_class_set_legacy_reset(dc, mcf_dtimer_reset);
    dc->user_creatable = true;
    device_class_set_props(dc, mcf_dtimer_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo mcf_dtimer_info = {
    .name = TYPE_MCF_DTIMER,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(McfDtimerState),
    .class_init = mcf_dtimer_class_init,
};

static void mcf_dtimer_register_types(void)
{
    type_register_static(&mcf_dtimer_info);
}

type_init(mcf_dtimer_register_types)
