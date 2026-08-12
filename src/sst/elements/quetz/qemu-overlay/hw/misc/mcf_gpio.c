/*
 * mcf_gpio.c -- Raptor ColdFire V4 GPIO (GPIOB0-3) model.
 *
 * A Path A QEMU device (mapped like mcf_dtimer) modeling the four Raptor GPIO
 * banks the BSP uses. Registers are 16-bit; the driver's value/direction write
 * semantics are asymmetric and must be reproduced exactly or every BSP GPIO
 * call reports failure on its readback check.
 *
 * Contract (raptor-bsp src/board_drivers/gpio/GPIO_driver.{c,h},
 * blockers.md 2). Bank bases 0x4000 apart; each register is accessed as a
 * uint16_t, so the byte offset is TWICE the driver's "offset (in short)" macro:
 *
 *   macro off(short)  byte off  register            width
 *   0                 +0x00     trigger level       16
 *   2                 +0x04     trigger type        16
 *   4                 +0x08     interrupt enable    16
 *   6                 +0x0C     interrupt status    16
 *   8                 +0x10     value               16
 *   12                +0x18     direction           16   (1=input, 0=output)
 *   14                +0x1C     pull enable         16
 *
 * Only the low 8 bits are meaningful (8 pins per bank).
 *
 * Write semantics (the asymmetry is real):
 *   - VALUE writes use mask-in-high-byte: the driver stores
 *     (bit_mask << 8) | data as one 16-bit word. The high byte selects which
 *     pins are affected; the low byte carries the data. Unmasked pins keep
 *     their previous value. No read-modify-write by the driver.
 *   - DIRECTION writes are plain: the driver does its own read-modify-write and
 *     stores the resulting 8-bit value with NO mask in the high byte.
 *   - Reads return the pin state in the low 8 bits (high byte reads 0).
 *
 * The setter functions re-read and compare (readback & mask) == (written &
 * mask), so writes MUST read back correctly.
 *
 * Interrupts: NOT delivered (blockers.md 2 / A5 -- no tester arms a GPIO
 * interrupt, and the GPIO event->vector assignment is unknown, so delivering
 * one would be a silent wrong answer). Per the blockers.md 2 policy table, the
 * trap is on the ARMING path only:
 *   - INT_ENABLE write non-zero  -> fail loudly (arms an undeliverable IRQ)
 *   - INT_ENABLE write zero      -> accept (a disable matches reality)
 *   - INT_ENABLE read            -> last written value
 *   - TRIGGER level/type write   -> fail loudly IF int-enable is non-zero;
 *                                   else accept as inert config
 *   - TRIGGER level/type read    -> last written value
 *   - INT_STATUS read            -> 0 (nothing can ever be pending)
 *   - INT_STATUS write (ack)     -> no-op
 *   - PULL_ENABLE read/write     -> plain storage (electrical; scoped out)
 *
 * Reset values are not specified by the BSP (blockers.md 2 "Still needed"); the
 * BSP never resets GPIO, so we pick 0 for value/direction and record that. The
 * GPIO_test oracle saves and restores direction around each pin, so a 0 reset
 * (all-output) is safe for it.
 */

#include "qemu/osdep.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"

#include "raptor_gpio_blocks.h"

#define TYPE_MCF_GPIO "mcf-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(McfGpioState, MCF_GPIO)

/* Register byte offsets within a bank (macro value x2; see the table above). */
#define GPIO_TRIGGER_LEVEL_OFFSET  0x00
#define GPIO_TRIGGER_TYPE_OFFSET   0x04
#define GPIO_INT_ENABLE_OFFSET     0x08
#define GPIO_INT_STATUS_OFFSET     0x0C
#define GPIO_VALUE_OFFSET          0x10
#define GPIO_DIRECTION_OFFSET      0x18
#define GPIO_PULL_ENABLE_OFFSET    0x1C

#define GPIO_PIN_MASK 0xFF             /* 8 pins per bank */

typedef struct McfGpioBank {
    struct McfGpioState *owner;
    char *name;
    uint64_t base;
    uint64_t size;
    MemoryRegion region;
    bool mapped;

    /* Low 8 bits meaningful for each. */
    uint8_t value;                     /* output latch / input level */
    uint8_t direction;                 /* 1 = input, 0 = output */
    uint8_t pull_enable;               /* electrical, plain storage */
    uint8_t int_enable;                /* stored; arming traps */
    uint8_t trigger_level;             /* stored */
    uint8_t trigger_type;              /* stored */
    bool arm_reported;                 /* de-dup the loud arming diagnostic */
} McfGpioBank;

struct McfGpioState {
    DeviceState parent_obj;
    char *target;
    McfGpioBank banks[G_N_ELEMENTS(raptor_gpio_blocks)];
};

static void gpio_report_arming(McfGpioBank *b, const char *what, uint64_t val)
{
    if (b->arm_reported) {
        return;
    }
    b->arm_reported = true;
    error_report("mcf-gpio: %s: %s (0x%" PRIx64 ") arms a GPIO interrupt that "
                 "cannot be delivered (event->vector assignment is unknown; "
                 "no BSP/tester uses GPIO interrupts). Treating as a loud "
                 "no-op; if an application genuinely needs GPIO IRQs, the INTC "
                 "line mapping must come from hardware docs (blockers.md 2).",
                 b->name, what, val);
}

static uint64_t gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    McfGpioBank *b = opaque;

    /* All GPIO registers are 16-bit accesses. Reject other widths loudly. */
    if (size != 2) {
        warn_report("mcf-gpio: %s: non-16-bit read at +0x%" PRIx64
                    " (size %u); returning 0", b->name, (uint64_t)offset, size);
        return 0;
    }

    switch (offset) {
    case GPIO_VALUE_OFFSET:
        return b->value & GPIO_PIN_MASK;
    case GPIO_DIRECTION_OFFSET:
        return b->direction & GPIO_PIN_MASK;
    case GPIO_PULL_ENABLE_OFFSET:
        return b->pull_enable & GPIO_PIN_MASK;
    case GPIO_INT_ENABLE_OFFSET:
        return b->int_enable & GPIO_PIN_MASK;
    case GPIO_TRIGGER_LEVEL_OFFSET:
        return b->trigger_level & GPIO_PIN_MASK;
    case GPIO_TRIGGER_TYPE_OFFSET:
        return b->trigger_type & GPIO_PIN_MASK;
    case GPIO_INT_STATUS_OFFSET:
        /* Nothing can ever be pending, so 0 is correct, not approximate. */
        return 0;
    default:
        break;
    }
    warn_report("mcf-gpio: %s: read at unmodeled offset +0x%" PRIx64
                "; returning 0", b->name, (uint64_t)offset);
    return 0;
}

static void gpio_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    McfGpioBank *b = opaque;
    uint16_t word = (uint16_t)value;

    if (size != 2) {
        warn_report("mcf-gpio: %s: non-16-bit write at +0x%" PRIx64
                    " (size %u, value 0x%" PRIx64 ") ignored",
                    b->name, (uint64_t)offset, size, value);
        return;
    }

    switch (offset) {
    case GPIO_VALUE_OFFSET: {
        /* mask-in-high-byte: high byte selects affected pins, low byte data.
         * Unmasked pins retain their prior value; no read-modify-write. */
        uint8_t mask = (word >> 8) & GPIO_PIN_MASK;
        uint8_t data = word & GPIO_PIN_MASK;
        b->value = (b->value & (uint8_t)~mask) | (data & mask);
        return;
    }
    case GPIO_DIRECTION_OFFSET:
        /* Plain store: the driver already did the read-modify-write and writes
         * the resulting 8-bit value with no mask in the high byte. */
        b->direction = word & GPIO_PIN_MASK;
        return;
    case GPIO_PULL_ENABLE_OFFSET:
        b->pull_enable = word & GPIO_PIN_MASK;
        return;
    case GPIO_INT_ENABLE_OFFSET:
        if (word & GPIO_PIN_MASK) {
            gpio_report_arming(b, "interrupt-enable write", word & GPIO_PIN_MASK);
        }
        /* Store either way so a read-back is consistent; a disable (zero) is a
         * genuine no-op, an enable is a loud no-op (not delivered). */
        b->int_enable = word & GPIO_PIN_MASK;
        return;
    case GPIO_TRIGGER_LEVEL_OFFSET:
        if (b->int_enable & GPIO_PIN_MASK) {
            gpio_report_arming(b, "trigger-level write with IRQ enabled", word);
        }
        b->trigger_level = word & GPIO_PIN_MASK;
        return;
    case GPIO_TRIGGER_TYPE_OFFSET:
        if (b->int_enable & GPIO_PIN_MASK) {
            gpio_report_arming(b, "trigger-type write with IRQ enabled", word);
        }
        b->trigger_type = word & GPIO_PIN_MASK;
        return;
    case GPIO_INT_STATUS_OFFSET:
        /* Ack of a never-pending interrupt: harmless no-op. */
        return;
    default:
        break;
    }
    warn_report("mcf-gpio: %s: write at unmodeled offset +0x%" PRIx64
                " (value 0x%" PRIx64 ") ignored",
                b->name, (uint64_t)offset, value);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    /* Registers are 16-bit; allow 1/2/4 at the region so a wrong-width probe
     * reaches the handler (which rejects it) instead of aborting. */
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl  = { .min_access_size = 1, .max_access_size = 4 },
};

static void gpio_bank_reset(McfGpioBank *b)
{
    b->value = 0x00;
    b->direction = 0x00;               /* all outputs; BSP never resets GPIO */
    b->pull_enable = 0x00;
    b->int_enable = 0x00;
    b->trigger_level = 0x00;
    b->trigger_type = 0x00;
    b->arm_reported = false;
}

static void mcf_gpio_realize(DeviceState *dev, Error **errp)
{
    McfGpioState *s = MCF_GPIO(dev);
    size_t i;

    if (!s->target || strcmp(s->target, RAPTOR_GPIO_TARGET)) {
        error_setg(errp, "mcf-gpio: only target="
                   RAPTOR_GPIO_TARGET " is supported");
        return;
    }

    for (i = 0; i < G_N_ELEMENTS(raptor_gpio_blocks); i++) {
        const RaptorGpioBlockDesc *d = &raptor_gpio_blocks[i];
        McfGpioBank *b = &s->banks[i];

        b->owner = s;
        b->name = g_strdup(d->name);
        b->base = d->base;
        b->size = d->size;
        gpio_bank_reset(b);

        memory_region_init_io(&b->region, OBJECT(dev), &gpio_ops, b,
                              b->name, b->size);
        memory_region_add_subregion_overlap(get_system_memory(), b->base,
                                            &b->region, 1);
        b->mapped = true;
    }
}

static void mcf_gpio_unrealize(DeviceState *dev)
{
    McfGpioState *s = MCF_GPIO(dev);
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(s->banks); i++) {
        McfGpioBank *b = &s->banks[i];
        if (b->mapped) {
            memory_region_del_subregion(get_system_memory(), &b->region);
            object_unparent(OBJECT(&b->region));
            b->mapped = false;
        }
        g_free(b->name);
        b->name = NULL;
    }
}

static Property mcf_gpio_properties[] = {
    DEFINE_PROP_STRING("target", McfGpioState, target),
    DEFINE_PROP_END_OF_LIST(),
};

static void mcf_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcf_gpio_realize;
    dc->unrealize = mcf_gpio_unrealize;
    dc->user_creatable = true;
    device_class_set_props(dc, mcf_gpio_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo mcf_gpio_info = {
    .name = TYPE_MCF_GPIO,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(McfGpioState),
    .class_init = mcf_gpio_class_init,
};

static void mcf_gpio_register_types(void)
{
    type_register_static(&mcf_gpio_info);
}

type_init(mcf_gpio_register_types)
