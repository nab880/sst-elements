#ifndef HW_M68K_RAPTOR_MULTICORE_H
#define HW_M68K_RAPTOR_MULTICORE_H

#include "hw/boards.h"
#include "cpu.h"

typedef struct RaptorSecondaryState {
    M68kCPU *cpu;
    uint32_t entry;
    uint32_t initial_sp;
    bool released;
} RaptorSecondaryState;

/* Called after the shared system memory map and primary CPU exist. */
void raptor_secondary_init(RaptorSecondaryState *s, MachineState *machine,
                           const char *filename, uint64_t ram_base,
                           uint64_t ram_size, uint32_t initial_sp);
/* GPIO output 7: high releases CPU1; low resets and holds it. */
void raptor_secondary_release(void *opaque, int line, int level);

#endif
