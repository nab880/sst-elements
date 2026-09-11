/*
 * Raptor eDMA functional subset. Register locations and TCD layout are observed
 * in the shipped BSP 5.7.0 ELF; transfer semantics follow MCF54455RM chapter 19.
 * See docs/raptor-edma.md for the evidence and deliberately unsupported modes.
 * RAM/ROM reads and RAM writes only: never invoke a peripheral's MMIO handler.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"

#define TYPE_RAPTOR_EDMA "raptor-edma"
OBJECT_DECLARE_SIMPLE_TYPE(RaptorEdmaState, RAPTOR_EDMA)
#define CHANNELS 16
#define TCD_BASE 0x1000
#define TCD_BYTES 32
#define MAX_MINOR_BYTES (1024 * 1024)
#define START 0x01
#define INTMAJOR 0x02
#define INTHALF 0x04
#define DREQ 0x08
#define ESG 0x10
#define MAJOR_LINK 0x20
#define ACTIVE 0x40
#define DONE 0x80
#define ES_SAE 0x80
#define ES_SOE 0x40
#define ES_DAE 0x20
#define ES_DOE 0x10
#define ES_NCE 0x08
#define ES_SBE 0x02
#define ES_DBE 0x01

typedef struct RaptorEdmaState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *timer;
    qemu_irq completion_irq, error_irq;
    bool completion_routed, error_routed;
    uint8_t tcd[CHANNELS][TCD_BYTES];
    uint8_t priority[CHANNELS];
    uint16_t erq, eei, interrupt, error, requests;
    uint32_t control, error_status;
    unsigned last_channel;
} RaptorEdmaState;

static void edma_unsupported(const char *what)
{
    error_report("RAPTOR_EDMA_UNSUPPORTED: %s", what);
    exit(EXIT_FAILURE);
}

static void edma_update_irq(RaptorEdmaState *s)
{
    qemu_set_irq(s->completion_irq, s->interrupt != 0);
    qemu_set_irq(s->error_irq, (s->error & s->eei) != 0);
}

static uint16_t edma_csr(RaptorEdmaState *s, unsigned channel)
{
    return lduw_be_p(s->tcd[channel] + 30);
}

static void edma_set_csr(RaptorEdmaState *s, unsigned channel, uint16_t csr)
{
    stw_be_p(s->tcd[channel] + 30, csr);
}

static bool edma_requested(RaptorEdmaState *s, unsigned ch)
{
    return (edma_csr(s, ch) & START) ||
           ((s->requests & s->erq) & (1u << ch));
}

static void edma_schedule(RaptorEdmaState *s)
{
    unsigned ch;
    if (timer_pending(s->timer)) {
        return;
    }
    for (ch = 0; ch < CHANNELS; ++ch) {
        if (edma_requested(s, ch)) {
            /* Functional scheduling point, deliberately not a bus-time claim. */
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
            return;
        }
    }
}

static bool edma_memory(uint32_t address, uint8_t *bytes, unsigned size,
                        bool write)
{
    hwaddr offset, length = size;
    MemoryRegion *mr;
    MemTxResult result;
    RCU_READ_LOCK_GUARD();

    if (size - 1 > UINT32_MAX - address) {
        return false;
    }
    mr = address_space_translate(&address_space_memory, address, &offset,
                                 &length, write, MEMTXATTRS_UNSPECIFIED);
    /* Validate before address_space_rw: Raptor's strict catch-all would abort
     * and device MMIO could have effects even when it later reports an error.
     * Reject ram devices too, since a flash device can have write callbacks. */
    if (length < size || !memory_region_is_ram(mr) ||
        memory_region_is_ram_device(mr) || mr->rom_device ||
        (write && mr->readonly)) {
        return false;
    }
    result = address_space_rw(&address_space_memory, address,
                              MEMTXATTRS_UNSPECIFIED, bytes, size, write);
    return result == MEMTX_OK;
}

static void edma_error(RaptorEdmaState *s, unsigned ch, uint32_t bits)
{
    s->error |= 1u << ch;
    s->error_status = (ch << 8) | bits;
    edma_set_csr(s, ch, edma_csr(s, ch) & ~(START | ACTIVE));
    edma_update_irq(s);
}

static unsigned edma_transfer_size(unsigned encoding)
{
    return encoding <= 2 ? 1u << encoding : encoding == 4 ? 16 : 0;
}

static uint32_t edma_advance(uint32_t address, int16_t offset, unsigned modulo)
{
    uint32_t next = address + offset;
    uint32_t mask = modulo ? (UINT32_MAX >> (32 - modulo)) : UINT32_MAX;
    return (address & ~mask) | (next & mask);
}

static void edma_minor(RaptorEdmaState *s, unsigned ch)
{
    uint8_t *t = s->tcd[ch];
    uint16_t csr = edma_csr(s, ch), attr = lduw_be_p(t + 4);
    uint16_t citer = lduw_be_p(t + 20), biter = lduw_be_p(t + 28);
    uint32_t source = ldl_be_p(t), destination = ldl_be_p(t + 16);
    uint32_t nbytes = ldl_be_p(t + 8), done;
    int16_t soff = lduw_be_p(t + 6), doff = lduw_be_p(t + 22);
    unsigned ssize = edma_transfer_size((attr >> 8) & 7);
    unsigned dsize = edma_transfer_size(attr & 7);
    unsigned smod = attr >> 11, dmod = (attr >> 3) & 31, chunk, index;
    uint8_t buffer[16];

    if (csr & (ESG | MAJOR_LINK | 0xff00) || (citer | biter) & 0x8000) {
        edma_unsupported("scatter/gather, linking and bandwidth-control TCDs are not supported");
    }
    if (s->priority[ch] & 0x80) {
        edma_unsupported("channel preemption is not supported");
    }
    if (!nbytes || nbytes > MAX_MINOR_BYTES) {
        edma_unsupported("minor loop exceeds the 1-MiB functional limit (zero means 4 GiB)");
    }
    if ((smod && (1ULL << smod) < ssize) ||
        (dmod && (1ULL << dmod) < dsize)) {
        edma_unsupported("modulo region smaller than a transfer is not supported");
    }
    csr = (csr & ~(START | DONE)) | ACTIVE;
    edma_set_csr(s, ch, csr);
    if (!ssize || !dsize || !citer || !biter || citer > biter ||
        nbytes % ssize || nbytes % dsize) {
        edma_error(s, ch, ES_NCE);
        return;
    }
    if (source % ssize) { edma_error(s, ch, ES_SAE); return; }
    if ((uint16_t)soff % ssize) { edma_error(s, ch, ES_SOE); return; }
    if (destination % dsize) { edma_error(s, ch, ES_DAE); return; }
    if ((uint16_t)doff % dsize) { edma_error(s, ch, ES_DOE); return; }
    chunk = MAX(ssize, dsize);
    for (done = 0; done < nbytes; done += chunk) {
        for (index = 0; index < chunk; index += ssize) {
            if (!edma_memory(source, buffer + index, ssize, false)) {
                edma_error(s, ch, ES_SBE);
                return;
            }
            source = edma_advance(source, soff, smod);
        }
        for (index = 0; index < chunk; index += dsize) {
            if (!edma_memory(destination, buffer + index, dsize, true)) {
                edma_error(s, ch, ES_DBE);
                return;
            }
            destination = edma_advance(destination, doff, dmod);
        }
    }
    --citer;
    if ((csr & INTHALF) && biter >= 2 && citer == (biter >> 1)) {
        s->interrupt |= 1u << ch;
    }
    if (!citer) {
        source += ldl_be_p(t + 12);
        destination += ldl_be_p(t + 24);
        csr |= DONE;
        if (csr & DREQ) { s->erq &= ~(1u << ch); }
        if (csr & INTMAJOR) { s->interrupt |= 1u << ch; }
        citer = biter;
    }
    stl_be_p(t, source);
    stl_be_p(t + 16, destination);
    stw_be_p(t + 20, citer);
    edma_set_csr(s, ch, csr & ~ACTIVE);
    edma_update_irq(s);
}

static void edma_tick(void *opaque)
{
    RaptorEdmaState *s = opaque;
    int selected = -1;
    unsigned n;
    for (n = 0; n < CHANNELS; ++n) {
        unsigned ch = s->control & 4 ? (s->last_channel + 1 + n) % CHANNELS : n;
        if (!edma_requested(s, ch)) { continue; }
        if (s->control & 4) { selected = ch; break; }
        if (selected < 0 || (s->priority[ch] & 15) > (s->priority[selected] & 15)) {
            selected = ch;
        }
    }
    if (selected >= 0) {
        if (!(s->control & 4)) {
            uint16_t used = 0;
            for (n = 0; n < CHANNELS; ++n) {
                uint16_t bit = 1u << (s->priority[n] & 15);
                if (used & bit) {
                    edma_unsupported("duplicate fixed priorities require unmodeled CPE semantics");
                }
                used |= bit;
            }
        }
        s->last_channel = selected;
        edma_minor(s, selected);
    }
    edma_schedule(s);
}

static bool edma_tcd_access(hwaddr addr, unsigned size)
{
    /* The published driver uses these exact register widths. Refuse byte and
     * combined-register writes until an additional access contract is tested. */
    unsigned off = addr & 31;
    return addr >= TCD_BASE && addr < TCD_BASE + CHANNELS * TCD_BYTES &&
        ((size == 4 && (off == 0 || off == 8 || off == 12 || off == 16 || off == 24)) ||
         (size == 2 && (off == 4 || off == 6 || off == 20 || off == 22 || off == 28 || off == 30)));
}

static uint64_t edma_read(void *opaque, hwaddr addr, unsigned size)
{
    RaptorEdmaState *s = opaque;
    if (edma_tcd_access(addr, size)) {
        uint8_t *p = s->tcd[(addr - TCD_BASE) / TCD_BYTES] + (addr & 31);
        return size == 4 ? ldl_be_p(p) : lduw_be_p(p);
    }
    if (addr == 0 && size == 4) { return s->control; }
    if (addr == 4 && size == 4) { return s->error_status | (s->error ? 0x80000000u : 0); }
    if (addr == 0x0e && size == 2) { return s->erq; }
    if (addr == 0x16 && size == 2) { return s->eei; }
    if (addr == 0x26 && size == 2) { return s->interrupt; }
    if (addr == 0x2e && size == 2) { return s->error; }
    if (addr >= 0x18 && addr <= 0x1f && size == 1) { return 0; }
    if (addr >= 0x100 && addr < 0x110 && size == 1) { return s->priority[addr - 0x100]; }
    edma_unsupported("unreviewed register read offset or width");
    return 0;
}

static void edma_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RaptorEdmaState *s = opaque;
    if (edma_tcd_access(addr, size)) {
        unsigned ch = (addr - TCD_BASE) / TCD_BYTES, off = addr & 31;
        uint8_t *p = s->tcd[ch] + off;
        if (off == 30) {
            if ((value & (INTMAJOR | INTHALF)) && !s->completion_routed) {
                edma_unsupported("completion interrupt armed without explicit functional routing");
            }
            value = (value & ~ACTIVE) | (edma_csr(s, ch) & ACTIVE);
            if (edma_csr(s, ch) & DONE) { value &= ~(ESG | MAJOR_LINK); }
        }
        if (size == 4) { stl_be_p(p, value); } else { stw_be_p(p, value); }
    } else if (addr == 0 && size == 4) {
        s->control = value & 6;
    } else if (addr == 0x0e && size == 2) {
        s->erq = value;
    } else if (addr == 0x16 && size == 2) {
        if (value && !s->error_routed) { edma_unsupported("error interrupt armed without explicit functional routing"); }
        s->eei = value;
    } else if (addr == 0x26 && size == 2) {
        s->interrupt &= ~value;
    } else if (addr == 0x2e && size == 2) {
        s->error &= ~value;
    } else if (addr >= 0x100 && addr < 0x110 && size == 1) {
        s->priority[addr - 0x100] = value & 0x8f;
    } else if (addr >= 0x18 && addr <= 0x1f && size == 1) {
        unsigned ch;
        uint16_t mask = value & 0x40 ? 0xffff : 1u << (value & 15);
        switch (addr) {
        case 0x18: s->erq |= mask; break;
        case 0x19: s->erq &= ~mask; break;
        case 0x1a:
            if (!s->error_routed) { edma_unsupported("error interrupt armed without explicit functional routing"); }
            s->eei |= mask;
            break;
        case 0x1b: s->eei &= ~mask; break;
        case 0x1c: s->interrupt &= ~mask; break;
        case 0x1d: s->error &= ~mask; break;
        case 0x1e:
        case 0x1f:
            for (ch = 0; ch < CHANNELS; ++ch) {
                if (mask & (1u << ch)) {
                    uint16_t csr = edma_csr(s, ch);
                    edma_set_csr(s, ch, addr == 0x1e ? csr | START : csr & ~DONE);
                }
            }
            break;
        }
    } else {
        edma_unsupported("unreviewed register write offset or width");
    }
    edma_update_irq(s);
    edma_schedule(s);
}

static void edma_request(void *opaque, int channel, int level)
{
    RaptorEdmaState *s = opaque;
    if (level) { s->requests |= 1u << channel; }
    else { s->requests &= ~(1u << channel); }
    edma_schedule(s);
}

static const MemoryRegionOps edma_ops = {
    .read = edma_read,
    .write = edma_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

static void edma_reset(DeviceState *dev)
{
    RaptorEdmaState *s = RAPTOR_EDMA(dev);
    unsigned ch;
    timer_del(s->timer);
    s->control = s->error_status = 0;
    /* Request inputs are externally driven levels, not resettable latches. */
    s->erq = s->eei = s->interrupt = s->error = 0;
    s->last_channel = CHANNELS - 1;
    memset(s->tcd, 0, sizeof(s->tcd));
    for (ch = 0; ch < CHANNELS; ++ch) { s->priority[ch] = ch; }
    edma_update_irq(s);
}

static void edma_init(Object *obj)
{
    RaptorEdmaState *s = RAPTOR_EDMA(obj);
    memory_region_init_io(&s->iomem, obj, &edma_ops, s, "raptor.edma", 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->completion_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->error_irq);
    qdev_init_gpio_in_named(DEVICE(obj), edma_request, "request", CHANNELS);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, edma_tick, s);
}

static void edma_finalize(Object *obj)
{
    timer_free(RAPTOR_EDMA(obj)->timer);
}

static Property edma_properties[] = {
    DEFINE_PROP_BOOL("completion-routed", RaptorEdmaState, completion_routed, false),
    DEFINE_PROP_BOOL("error-routed", RaptorEdmaState, error_routed, false),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_edma = {
    .name = "raptor-edma",
    .unmigratable = true,
};

static void edma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, edma_reset);
    device_class_set_props(dc, edma_properties);
    /* Timer/descriptor migration is deliberately unsupported. */
    dc->vmsd = &vmstate_edma;
    dc->user_creatable = false;
}

static const TypeInfo edma_info = {
    .name = TYPE_RAPTOR_EDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RaptorEdmaState),
    .instance_init = edma_init,
    .instance_finalize = edma_finalize,
    .class_init = edma_class_init,
};

static void edma_register_types(void) { type_register_static(&edma_info); }
type_init(edma_register_types)
