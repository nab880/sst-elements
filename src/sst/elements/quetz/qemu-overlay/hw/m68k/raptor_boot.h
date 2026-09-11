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

/* Register once after mapping ROM/RAM; board-owned state must outlive the reset callback. */
void raptor_boot_init(RaptorBootState *state, MachineState *machine,
                      M68kCPU *cpu, hwaddr rom_base, uint64_t rom_size);

#endif
