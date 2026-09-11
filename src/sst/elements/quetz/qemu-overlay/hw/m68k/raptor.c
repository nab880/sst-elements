/*
 * Raptor Core2 functional-profile machine.
 *
 * This machine models the reviewed BSP-visible subset.  It deliberately does
 * not claim an exact orderable CPU or complete interrupt topology.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "cpu.h"
#include "elf.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/m68k/mcf.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qom/object.h"
#include "sysemu/qtest.h"
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"
#include "raptor_boot.h"
#include "raptor_multicore.h"
#include "hw/irq.h"

#define TYPE_RAPTOR_MACHINE MACHINE_TYPE_NAME("raptor-core2")
OBJECT_DECLARE_SIMPLE_TYPE(RaptorMachineState, RAPTOR_MACHINE)

#define RAPTOR_ROM_BASE       0x00000000ULL
#define RAPTOR_ROM_SIZE       (64 * MiB)
#define RAPTOR_MPFLASH_BASE   0x05000000ULL
#define RAPTOR_MPFLASH_SIZE   (512 * KiB)
#define RAPTOR_TEST_BASE      0x06000000ULL
#define RAPTOR_TEST_SIZE      (64 * KiB)
#define RAPTOR_SRAM1_BASE     0x07000000ULL
#define RAPTOR_SRAM_SIZE      (2 * MiB)
#define RAPTOR_SRAM2_BASE     0x08000000ULL
#define RAPTOR_P2_BASE        0x40000000ULL
#define RAPTOR_P1_BASE        0x80000000ULL
#define RAPTOR_LOCAL_RAM_SIZE (64 * KiB)

#define RAPTOR_FLEXBUS_BASE   0xfc008000ULL
#define RAPTOR_PLATFORM_BASE  0xfc040000ULL
#define RAPTOR_EDMA_BASE      0xfc044000ULL
#define RAPTOR_INTC_BASE      0xfc048000ULL
#define RAPTOR_UART0_BASE     0xfc060000ULL
#define RAPTOR_UART1_BASE     0xfc064000ULL
#define RAPTOR_UART2_BASE     0xfc068000ULL
typedef struct RaptorFlexBus {
    MemoryRegion iomem;
    struct RaptorMachineState *machine;
    uint32_t regs[3];
} RaptorFlexBus;

typedef struct RaptorPlatform {
    MemoryRegion iomem;
    struct RaptorMachineState *machine;
} RaptorPlatform;

struct RaptorMachineState {
    MachineState parent_obj;
    bool strict_mmio;
    uint32_t edma_irq;
    uint32_t edma_error_irq;
    char *secondary_kernel;
    MemoryRegion unknown;
    MemoryRegion rom;
    MemoryRegion mpflash;
    MemoryRegion test_ram;
    MemoryRegion sram1;
    MemoryRegion sram2;
    MemoryRegion p1_ram;
    RaptorFlexBus flexbus;
    RaptorPlatform platform;
    RaptorBootState boot;
    RaptorSecondaryState secondary;
};

static void raptor_bad_access(RaptorMachineState *s, const char *owner,
                              hwaddr addr, unsigned size, bool write)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "RAPTOR_MMIO_UNSUPPORTED owner=%s op=%s "
                  "addr=0x%08" HWADDR_PRIx
                  " size=%u\n",
                  owner, write ? "write" : "read", addr, size);
    if (s->strict_mmio) {
        error_report("RAPTOR_MMIO_UNSUPPORTED strict rejection owner=%s "
                     "addr=0x%08" HWADDR_PRIx " size=%u", owner, addr, size);
        exit(EXIT_FAILURE);
    }
}

static uint64_t raptor_unknown_read(void *opaque, hwaddr addr, unsigned size)
{
    RaptorMachineState *s = opaque;

    raptor_bad_access(s, "unmapped", addr, size, false);
    return 0;
}

static void raptor_unknown_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    RaptorMachineState *s = opaque;

    (void)value;
    raptor_bad_access(s, "unmapped", addr, size, true);
}

static const MemoryRegionOps raptor_unknown_ops = {
    .read = raptor_unknown_read,
    .write = raptor_unknown_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static uint64_t raptor_flexbus_read(void *opaque, hwaddr addr, unsigned size)
{
    RaptorFlexBus *s = opaque;

    if (size == 4 && addr <= 8 && !(addr & 3)) {
        return s->regs[addr >> 2];
    }
    raptor_bad_access(s->machine, "flexbus", RAPTOR_FLEXBUS_BASE + addr,
                      size, false);
    return 0;
}

static void raptor_flexbus_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    RaptorFlexBus *s = opaque;

    if (size == 4 && addr <= 8 && !(addr & 3)) {
        s->regs[addr >> 2] = value;
        return;
    }
    raptor_bad_access(s->machine, "flexbus", RAPTOR_FLEXBUS_BASE + addr,
                      size, true);
}

static const MemoryRegionOps raptor_flexbus_ops = {
    .read = raptor_flexbus_read,
    .write = raptor_flexbus_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static uint64_t raptor_platform_read(void *opaque, hwaddr addr, unsigned size)
{
    RaptorPlatform *s = opaque;

    if (addr == 2 && size == 2) {
        return 0;
    }
    if (addr == 0xf && size == 1) {
        return 0;
    }
    raptor_bad_access(s->machine, "platform", RAPTOR_PLATFORM_BASE + addr,
                      size, false);
    return 0;
}

static void raptor_platform_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    RaptorPlatform *s = opaque;

    (void)value;
    if (addr == 0xf && size == 1) {
        return;
    }
    raptor_bad_access(s->machine, "platform", RAPTOR_PLATFORM_BASE + addr,
                      size, true);
}

static const MemoryRegionOps raptor_platform_ops = {
    .read = raptor_platform_read,
    .write = raptor_platform_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
        .unaligned = false,
    },
};

static void raptor_peripherals_reset(void *opaque)
{
    RaptorMachineState *s = opaque;

    memset(s->flexbus.regs, 0, sizeof(s->flexbus.regs));
}

static void raptor_map_ram(MemoryRegion *mr, const char *name, uint64_t base,
                           uint64_t size)
{
    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

static DeviceState *raptor_create_reviewed_device(MachineState *machine,
                                                  const char *name,
                                                  const char *type,
                                                  bool strict_mmio)
{
    DeviceState *dev = qdev_new(type);

    object_property_add_child(OBJECT(machine), name, OBJECT(dev));
    qdev_prop_set_string(dev, "target", "raptor");
    qdev_prop_set_bit(dev, "strict-mmio", strict_mmio);
    qdev_realize_and_unref(dev, NULL, &error_fatal);
    /* These reviewed devices have no parent bus, so QOM ownership alone does
     * not enroll them in the machine reset tree. */
    qemu_register_resettable(OBJECT(dev));
    return dev;
}

static void raptor_create_edma(RaptorMachineState *s, qemu_irq *pic)
{
    DeviceState *dev;
    if (s->edma_irq && s->edma_irq == s->edma_error_irq) {
        error_report("Raptor functional eDMA completion/error routes must differ");
        exit(EXIT_FAILURE);
    }
    dev = qdev_new("raptor-edma");
    object_property_add_child(OBJECT(s), "edma", OBJECT(dev));
    qdev_prop_set_bit(dev, "completion-routed", s->edma_irq != 0);
    qdev_prop_set_bit(dev, "error-routed", s->edma_error_irq != 0);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, RAPTOR_EDMA_BASE);
    if (s->edma_irq) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[s->edma_irq]);
    }
    if (s->edma_error_irq) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1, pic[s->edma_error_irq]);
    }
}

static void raptor_get_edma_route(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    RaptorMachineState *s = RAPTOR_MACHINE(obj);
    uint64_t value = opaque ? s->edma_error_irq : s->edma_irq;
    visit_type_uint64(v, name, &value, errp);
}

static void raptor_set_edma_route(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    RaptorMachineState *s = RAPTOR_MACHINE(obj);
    uint64_t value;
    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (value > 63 || (value >= 26 && value <= 28) || value == 30) {
        error_setg(errp, "%s requires 0 (disabled) or an unused source 1..63; "
                   "UART 26..28 and reference FFT 30 are reserved", name);
        return;
    }
    if (opaque) {
        s->edma_error_irq = value;
    } else {
        s->edma_irq = value;
    }
}

static void raptor_machine_init(MachineState *machine)
{
    RaptorMachineState *s = RAPTOR_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    M68kCPU *cpu;
    CPUM68KState *env;
    qemu_irq *pic;
    DeviceState *gpio;

    if (machine->ram_size != RAPTOR_LOCAL_RAM_SIZE) {
        error_report("raptor-core2 requires exactly 64 KiB of P2 local RAM");
        exit(EXIT_FAILURE);
    }
    if ((machine->smp.cpus == 2) !=
        (s->secondary_kernel && s->secondary_kernel[0])) {
        error_report("raptor-core2 requires -smp 2 together with secondary-kernel=ELF");
        exit(EXIT_FAILURE);
    }

    cpu = M68K_CPU(cpu_create(machine->cpu_type));
    env = &cpu->env;
    env->vbr = 0;

    memory_region_init_io(&s->unknown, OBJECT(machine), &raptor_unknown_ops, s,
                          "raptor.unmapped", UINT64_C(1) << 32);
    memory_region_add_subregion_overlap(sysmem, 0, &s->unknown, -1000);

    memory_region_init_rom(&s->rom, NULL, "raptor.rom",
                           RAPTOR_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, RAPTOR_ROM_BASE, &s->rom);
    raptor_map_ram(&s->mpflash, "raptor.mpflash", RAPTOR_MPFLASH_BASE,
                   RAPTOR_MPFLASH_SIZE);
    raptor_map_ram(&s->test_ram, "raptor.test-ram", RAPTOR_TEST_BASE,
                   RAPTOR_TEST_SIZE);
    raptor_map_ram(&s->sram1, "raptor.sram1", RAPTOR_SRAM1_BASE,
                   RAPTOR_SRAM_SIZE);
    raptor_map_ram(&s->sram2, "raptor.sram2", RAPTOR_SRAM2_BASE,
                   RAPTOR_SRAM_SIZE);
    memory_region_add_subregion(sysmem, RAPTOR_P2_BASE, machine->ram);
    raptor_map_ram(&s->p1_ram, "raptor.p1-ram", RAPTOR_P1_BASE,
                   RAPTOR_LOCAL_RAM_SIZE);

    s->flexbus.machine = s;
    memory_region_init_io(&s->flexbus.iomem, OBJECT(machine),
                          &raptor_flexbus_ops, &s->flexbus,
                          "raptor.flexbus", 0x4000);
    memory_region_add_subregion(sysmem, RAPTOR_FLEXBUS_BASE,
                                &s->flexbus.iomem);

    s->platform.machine = s;
    memory_region_init_io(&s->platform.iomem, OBJECT(machine),
                          &raptor_platform_ops, &s->platform,
                          "raptor.platform", 0x4000);
    memory_region_add_subregion(sysmem, RAPTOR_PLATFORM_BASE,
                                &s->platform.iomem);

    pic = mcf_intc_init_ext(sysmem, RAPTOR_INTC_BASE, cpu, true);
    mcf_uart_create_mmap(RAPTOR_UART0_BASE, pic[26], serial_hd(0));
    mcf_uart_create_mmap(RAPTOR_UART1_BASE, pic[27], serial_hd(1));
    mcf_uart_create_mmap(RAPTOR_UART2_BASE, pic[28], serial_hd(2));
    raptor_create_edma(s, pic);

    /*
     * GPIO and DTIMER behavior comes from the same generated board-contract
     * tables used by the legacy compatibility path. The dedicated machine
     * owns these instances; the launcher refuses a second profile overlay.
     */
    gpio = raptor_create_reviewed_device(machine, "gpio", "mcf-gpio", s->strict_mmio);
    raptor_create_reviewed_device(machine, "dtimer", "mcf-dtimer", s->strict_mmio);

    g_free(pic);
    qemu_register_reset(raptor_peripherals_reset, s);
    raptor_peripherals_reset(s);

    raptor_boot_init(&s->boot, machine, cpu, RAPTOR_ROM_BASE, RAPTOR_ROM_SIZE);
    if (machine->smp.cpus == 2) {
        raptor_secondary_init(&s->secondary, machine, s->secondary_kernel,
                               RAPTOR_P2_BASE, RAPTOR_LOCAL_RAM_SIZE, 0x4000fc00);
        qdev_connect_gpio_out_named(gpio, "out", 7,
            qemu_allocate_irq(raptor_secondary_release, &s->secondary, 0));
    }
}

static char *raptor_get_secondary_kernel(Object *obj, Error **errp)
{
    return g_strdup(RAPTOR_MACHINE(obj)->secondary_kernel);
}

static void raptor_set_secondary_kernel(Object *obj, const char *value,
                                         Error **errp)
{
    RaptorMachineState *s = RAPTOR_MACHINE(obj);
    g_free(s->secondary_kernel);
    s->secondary_kernel = g_strdup(value);
}

static bool raptor_get_strict_mmio(Object *obj, Error **errp)
{
    return RAPTOR_MACHINE(obj)->strict_mmio;
}

static void raptor_set_strict_mmio(Object *obj, bool value, Error **errp)
{
    RAPTOR_MACHINE(obj)->strict_mmio = value;
}

static void raptor_machine_instance_init(Object *obj)
{
    RAPTOR_MACHINE(obj)->strict_mmio = true;
}

static void raptor_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Raptor Core2 functional-v1 profile";
    mc->init = raptor_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("cfv4e");
    mc->default_ram_id = "raptor.p2-ram";
    mc->default_ram_size = RAPTOR_LOCAL_RAM_SIZE;
    mc->min_cpus = 1;
    mc->max_cpus = 2;
    mc->default_cpus = 1;

    object_class_property_add_bool(oc, "strict-mmio", raptor_get_strict_mmio,
                                   raptor_set_strict_mmio);
    object_class_property_add_str(oc, "secondary-kernel",
                                  raptor_get_secondary_kernel,
                                  raptor_set_secondary_kernel);
    object_class_property_set_description(oc, "secondary-kernel",
        "P2 ELF for the optional second CPU, held until GPIOB0 output bit 7 releases it");
    object_class_property_set_description(
        oc, "strict-mmio",
        "Fail immediately on accesses outside the reviewed subset");
    object_class_property_add(oc, "edma-irq", "uint64",
                              raptor_get_edma_route, raptor_set_edma_route,
                              NULL, NULL);
    object_class_property_add(oc, "edma-error-irq", "uint64",
                              raptor_get_edma_route, raptor_set_edma_route,
                              NULL, (void *)1);
    object_class_property_set_description(oc, "edma-irq",
        "Explicit functional aggregate completion route; 0 disables, not silicon routing");
    object_class_property_set_description(oc, "edma-error-irq",
        "Explicit functional aggregate error route; 0 disables, not silicon routing");
}

static const TypeInfo raptor_machine_typeinfo = {
    .name = TYPE_RAPTOR_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(RaptorMachineState),
    .instance_init = raptor_machine_instance_init,
    .class_init = raptor_machine_class_init,
};

static void raptor_machine_register_types(void)
{
    type_register_static(&raptor_machine_typeinfo);
}

type_init(raptor_machine_register_types)
