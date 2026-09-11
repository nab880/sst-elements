/*
 * Raptor functional boot and reset support.
 *
 * The BSP confirms the ROM aperture and the big-endian SP/PC vector layout.
 * Physical power-on vector selection and flash programming are not specified.
 * Therefore raw ROM boot requires an explicit -bios image; the default path
 * remains direct loading of an unchanged application ELF via -kernel.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "elf.h"
#include "exec/address-spaces.h"
#include "hw/loader.h"
#include "sysemu/qtest.h"
#include "sysemu/reset.h"
#include "raptor_boot.h"

static void raptor_boot_reset(void *opaque)
{
    RaptorBootState *state = opaque;

    cpu_reset(CPU(state->cpu));
    state->cpu->env.pc = state->entry;
    if (state->vector_boot) {
        state->cpu->env.aregs[7] = state->initial_sp;
    }
}

static bool raptor_boot_stack_is_writable(uint32_t sp)
{
    MemoryRegionSection section;
    bool writable;

    if (sp < 4 || (sp & 3)) {
        return false;
    }
    section = memory_region_find(get_system_memory(), sp - 4, 4);
    writable = int128_eq(section.size, int128_make64(4)) &&
               memory_region_is_ram(section.mr) &&
               !memory_region_is_rom(section.mr);
    memory_region_unref(section.mr);
    return writable;
}

static void raptor_boot_load_rom(RaptorBootState *state, const char *filename,
                                 hwaddr rom_base, uint64_t rom_size)
{
    g_autofree char *bytes = NULL;
    g_autoptr(GError) error = NULL;
    gsize size;
    uint32_t entry;
    uint32_t sp;

    if (!g_file_get_contents(filename, &bytes, &size, &error)) {
        error_report("Raptor ROM image '%s': %s", filename, error->message);
        exit(EXIT_FAILURE);
    }
    if (size < 10 || size > rom_size) {
        error_report("Raptor ROM image must contain SP/PC vectors and code "
                     "and fit the configured ROM aperture");
        exit(EXIT_FAILURE);
    }
    sp = ldl_be_p(bytes);
    entry = ldl_be_p(bytes + 4);
    if ((entry & 1) || entry < rom_base + 8 || entry > rom_base + size - 2) {
        error_report("Raptor ROM reset PC 0x%08x must point to aligned code "
                     "inside the loaded image", entry);
        exit(EXIT_FAILURE);
    }
    if (!raptor_boot_stack_is_writable(sp)) {
        error_report("Raptor ROM reset SP 0x%08x must be aligned above "
                     "mapped writable memory", sp);
        exit(EXIT_FAILURE);
    }
    rom_add_blob_fixed(filename, bytes, size, rom_base);
    state->vector_boot = true;
    state->entry = entry;
    state->initial_sp = sp;
}

void raptor_boot_init(RaptorBootState *state, MachineState *machine,
                      M68kCPU *cpu, hwaddr rom_base, uint64_t rom_size)
{
    uint64_t entry, low, high;
    ssize_t size;

    memset(state, 0, sizeof(*state));
    state->cpu = cpu;
    if (machine->firmware && machine->kernel_filename) {
        error_report("Raptor boot requires either -kernel ELF or -bios ROM, "
                     "not both");
        exit(EXIT_FAILURE);
    }
    if (machine->firmware) {
        raptor_boot_load_rom(state, machine->firmware, rom_base, rom_size);
    } else if (machine->kernel_filename) {
        size = load_elf(machine->kernel_filename, NULL, NULL, NULL,
                        &entry, &low, &high, NULL, 1, EM_68K, 0, 0);
        if (size < 0) {
            error_report("Could not load Raptor ELF '%s': %s",
                         machine->kernel_filename, load_elf_strerror(size));
            exit(EXIT_FAILURE);
        }
        /* The explicit two-core functional profile assigns the primary
         * image to P1 and the secondary image to P2. Reject overlapping or
         * external primary segments before a second image can replace them.
         * Single-core loading retains its existing address flexibility. */
        if (machine->smp.cpus == 2 &&
            (size == 0 || low >= high || low < 0x80000000ULL || high > 0x80010000ULL ||
             entry < 0x80000000ULL || entry >= 0x80010000ULL || (entry & 1))) {
            error_report("Raptor two-core primary ELF segments and entry "
                         "must be confined to P1 RAM 0x80000000..0x8000ffff");
            exit(EXIT_FAILURE);
        }
        state->entry = entry;
    } else if (!qtest_enabled()) {
        error_report("Raptor requires -kernel ELF or explicit -bios ROM");
        exit(EXIT_FAILURE);
    }
    qemu_register_reset_nosnapshotload(raptor_boot_reset, state);
    raptor_boot_reset(state);
}
