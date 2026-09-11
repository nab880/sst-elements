/* Explicit functional boot configuration; no silicon reset-source claim. */
#ifndef QUETZ_RAPTOR_BOOT_H
#define QUETZ_RAPTOR_BOOT_H

#include "cpu.h"
#include "hw/boards.h"

typedef struct RaptorBootState {
    M68kCPU *cpu;
    uint32_t entry;
    uint32_t initial_sp;
    bool vector_boot;
} RaptorBootState;

/* Call after mapping ROM and writable memory, once for the boot CPU only.
 * -kernel keeps direct ELF entry; -bios opts into a raw ROM SP/PC header.
 * The board owns state for the lifetime of the registered reset callback. */
void raptor_boot_init(RaptorBootState *state, MachineState *machine,
                      M68kCPU *cpu, hwaddr rom_base, uint64_t rom_size);

#endif
