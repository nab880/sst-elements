/*
 * sst_mmio_bridge.c — QEMU device: synchronous MMIO via Quetz shmem IPC.
 *
 * Plain TYPE_DEVICE (not SysBusDevice) so it can be instantiated with
 * `-device sst-mmio-bridge,shmname=...,base=...,size=...` on any machine.
 * Realize maps a MemoryRegion at `base` directly into system memory.
 *
 * Reverse (SST -> guest) IRQ injection: with irq-count=N > 0, a periodic
 * QEMU_CLOCK_VIRTUAL timer drains the shared-memory IRQ slots (see
 * quetz_ipc_client.h) and forwards each level change to interrupt-controller
 * input `line` via qdev_get_gpio_in(). The controller is resolved by QOM
 * type (default "mcf-intc", whose 64 inputs the overlay exposes as qdev
 * GPIOs); other machines can name a controller type through the intc-type
 * property, provided that device registers qdev GPIO inputs.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

#include <errno.h>

#include "quetz/quetz_ipc_client.h"
#include "quetz/quetz_ipc_types.h"

#define TYPE_SST_MMIO_BRIDGE "sst-mmio-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(SstMmioBridgeState, SST_MMIO_BRIDGE)

struct SstMmioBridgeState {
    DeviceState parent_obj;
    MemoryRegion mmio;
    QuetzIpcClient *ipc;
    char *shmname;
    uint64_t base;
    uint64_t size;
    uint32_t vcpu_id;
    bool mapped;

    /* SST-device IRQ injection (disabled when irq_count == 0). */
    uint32_t irq_count;
    uint64_t irq_poll_ns;
    char *intc_type;
    QEMUTimer *irq_timer;
    qemu_irq *irqs;
};

static uint64_t sst_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    SstMmioBridgeState *s = opaque;
    return quetz_ipc_mmio_read(s->ipc, s->vcpu_id, s->base + offset, size);
}

static void sst_mmio_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    SstMmioBridgeState *s = opaque;
    quetz_ipc_mmio_write(s->ipc, s->vcpu_id, s->base + offset, size, value);
}

static const MemoryRegionOps sst_mmio_ops = {
    .read = sst_mmio_read,
    .write = sst_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl  = { .min_access_size = 1, .max_access_size = 8 },
};

/* Timer callback (main loop, BQL held): drain SST's IRQ level changes and
 * apply them to the interrupt controller, then re-arm. */
static void sst_mmio_bridge_irq_poll(void *opaque)
{
    SstMmioBridgeState *s = opaque;
    QuetzIrqChange changes[QUETZ_MAX_IRQ_LINES];
    unsigned n;

    do {
        n = quetz_ipc_irq_drain(s->ipc, s->irq_count, changes,
                                QUETZ_MAX_IRQ_LINES);
        for (unsigned i = 0; i < n; i++) {
            /* The bridge wires a single INTC, fed from mailbox row 0. A
             * change posted to another vcore's row must not clobber row 0's
             * level on the same line (last-writer-wins on one GPIO). */
            if (changes[i].vcore != 0) {
                warn_report_once("sst-mmio-bridge: dropping IRQ line %u "
                                 "change posted to vcore %u (only vcore 0 "
                                 "is wired to the INTC)",
                                 changes[i].line, changes[i].vcore);
                continue;
            }
            qemu_set_irq(s->irqs[changes[i].line], changes[i].level != 0);
        }
    } while (n == QUETZ_MAX_IRQ_LINES);

    timer_mod(s->irq_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->irq_poll_ns);
}

static void sst_mmio_bridge_irq_init(SstMmioBridgeState *s, Error **errp)
{
    const char *intc_type =
        (s->intc_type && s->intc_type[0]) ? s->intc_type : "mcf-intc";
    Object *intc;
    bool ambiguous = false;

    if (s->irq_count > QUETZ_MAX_IRQ_LINES) {
        error_setg(errp, "sst-mmio-bridge: irq-count %u exceeds the shared "
                   "IRQ mailbox size %u", s->irq_count, QUETZ_MAX_IRQ_LINES);
        return;
    }

    intc = object_resolve_path_type("", intc_type, &ambiguous);
    if (!intc || ambiguous) {
        error_setg(errp, "sst-mmio-bridge: irq-count=%u needs exactly one "
                   "'%s' device on this machine (%s)", s->irq_count,
                   intc_type, ambiguous ? "ambiguous" : "none found");
        return;
    }

    s->irqs = g_new0(qemu_irq, s->irq_count);
    for (uint32_t i = 0; i < s->irq_count; i++) {
        s->irqs[i] = qdev_get_gpio_in(DEVICE(intc), i);
    }

    /* Poll on virtual time: it keeps running while the guest sits in `stop`
     * (low-power wait), which is exactly when SST devices raise lines. The
     * latency here is functional, not modeled. */
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                sst_mmio_bridge_irq_poll, s);
    timer_mod(s->irq_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->irq_poll_ns);
}

static void sst_mmio_bridge_realize(DeviceState *dev, Error **errp)
{
    SstMmioBridgeState *s = SST_MMIO_BRIDGE(dev);

    if (!s->shmname || !s->shmname[0]) {
        error_setg(errp, "sst-mmio-bridge: shmname property required");
        return;
    }
    if (s->size == 0) {
        error_setg(errp, "sst-mmio-bridge: size must be > 0");
        return;
    }

    s->ipc = quetz_ipc_attach(s->shmname);
    if (!s->ipc) {
        error_setg(errp,
                   "sst-mmio-bridge: failed to attach shmem '%s' (errno=%d)",
                   s->shmname, errno);
        return;
    }

    memory_region_init_io(&s->mmio, OBJECT(dev), &sst_mmio_ops, s,
                          TYPE_SST_MMIO_BRIDGE, s->size);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        s->base, &s->mmio, 1);
    s->mapped = true;

    if (s->irq_count > 0) {
        sst_mmio_bridge_irq_init(s, errp);
    }
}

static void sst_mmio_bridge_unrealize(DeviceState *dev)
{
    SstMmioBridgeState *s = SST_MMIO_BRIDGE(dev);
    if (s->irq_timer) {
        timer_free(s->irq_timer);
        s->irq_timer = NULL;
    }
    g_free(s->irqs);
    s->irqs = NULL;
    if (s->mapped) {
        memory_region_del_subregion(get_system_memory(), &s->mmio);
        s->mapped = false;
    }
    if (s->ipc) {
        quetz_ipc_detach(s->ipc);
        s->ipc = NULL;
    }
}

static Property sst_mmio_bridge_properties[] = {
    DEFINE_PROP_STRING("shmname", SstMmioBridgeState, shmname),
    DEFINE_PROP_UINT64("base", SstMmioBridgeState, base, 0),
    DEFINE_PROP_UINT64("size", SstMmioBridgeState, size, 0x400),
    DEFINE_PROP_UINT32("vcpu_id", SstMmioBridgeState, vcpu_id, 0),
    /* SST-device IRQ injection: poll lines [0, irq-count) of the shared IRQ
     * mailbox (0 = off). intc-type names the QOM type whose qdev GPIO inputs
     * receive the lines. */
    DEFINE_PROP_UINT32("irq-count", SstMmioBridgeState, irq_count, 0),
    DEFINE_PROP_UINT64("irq-poll-ns", SstMmioBridgeState, irq_poll_ns, 10000),
    DEFINE_PROP_STRING("intc-type", SstMmioBridgeState, intc_type),
    DEFINE_PROP_END_OF_LIST(),
};

static void sst_mmio_bridge_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sst_mmio_bridge_realize;
    dc->unrealize = sst_mmio_bridge_unrealize;
    dc->user_creatable = true;
    device_class_set_props(dc, sst_mmio_bridge_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo sst_mmio_bridge_info = {
    .name          = TYPE_SST_MMIO_BRIDGE,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(SstMmioBridgeState),
    .class_init    = sst_mmio_bridge_class_init,
};

static void sst_mmio_bridge_register_types(void)
{
    type_register_static(&sst_mmio_bridge_info);
}

type_init(sst_mmio_bridge_register_types)
