/* Functional second Raptor CPU with shared system memory and GPIO reset. */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/core/cpu.h"
#include "hw/loader.h"
#include "sysemu/cpus.h"
#include "sysemu/reset.h"
#include "elf.h"
#include "raptor_multicore.h"

static void raptor_secondary_reset(void *opaque)
{
    RaptorSecondaryState *s = opaque;
    CPUState *cpu = CPU(s->cpu);
    cpu_reset(cpu);
    s->cpu->env.pc = s->entry;
    s->cpu->env.aregs[7] = s->initial_sp;
    cpu->halted = true;
    s->released = false;
    qemu_cpu_kick(cpu);
}

void raptor_secondary_release(void *opaque, int line, int level)
{
    (void)line;
    RaptorSecondaryState *s = opaque;
    if (!level) {
        raptor_secondary_reset(s);
    } else if (!s->released) {
        CPUState *cpu = CPU(s->cpu);
        /* A release always restarts the secondary ELF, including after a
         * previous release/hold cycle. RAM stays shared and is not cleared. */
        cpu_reset(cpu);
        s->cpu->env.pc = s->entry;
        s->cpu->env.aregs[7] = s->initial_sp;
        s->released = true;
        cpu->halted = false;
        qemu_cpu_kick(cpu);
    }
}

void raptor_secondary_init(RaptorSecondaryState *s, MachineState *machine,
                           const char *filename, uint64_t ram_base,
                           uint64_t ram_size, uint32_t initial_sp)
{
    uint64_t entry = 0, low = 0, high = 0;
    if (strcmp(machine->cpu_type, M68K_CPU_TYPE_NAME("cfv4e"))) {
        error_report("raptor-core2: two CPUs require the cfv4e CPU model");
        exit(EXIT_FAILURE);
    }
    if (machine->smp.cpus != 2 || !filename || !filename[0] ||
        !ram_size || ram_base > UINT32_MAX || ram_size > UINT32_MAX - ram_base ||
        initial_sp < ram_base || initial_sp >= ram_base + ram_size ||
        (initial_sp & 3)) {
        error_report("raptor-core2: two CPUs require a secondary-kernel ELF and valid P2 stack");
        exit(EXIT_FAILURE);
    }
    if (qemu_tcg_mttcg_enabled()) {
        error_report("raptor-core2: two CPUs require -accel tcg,thread=single");
        exit(EXIT_FAILURE);
    }
    if (load_elf(filename, NULL, NULL, NULL, &entry, &low, &high,
                 NULL, 1, EM_68K, 0, 0) < 0 || low < ram_base ||
        high > ram_base + ram_size || high <= low || entry < low || entry >= high ||
        (entry & 1)) {
        error_report("raptor-core2: secondary-kernel must be a ColdFire ELF entirely inside P2 RAM");
        exit(EXIT_FAILURE);
    }
    s->entry = entry;
    s->initial_sp = initial_sp;
    s->cpu = M68K_CPU(cpu_create(machine->cpu_type));
    raptor_secondary_reset(s);
    qemu_register_reset(raptor_secondary_reset, s);
}
