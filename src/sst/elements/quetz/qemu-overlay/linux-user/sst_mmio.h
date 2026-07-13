#ifndef SST_MMIO_H
#define SST_MMIO_H

#include "qemu/osdep.h"
#include "exec/cpu_ldst.h"
#include "hw/core/cpu.h"

struct SstMmioRange {
    char     shmname[256];
    uint64_t base;
    uint64_t size;
    unsigned vcpu_id;
};

/* Parse one -sst-mmio-range SPEC (shmname=,base=,size=,vcpu_id=). Repeatable. */
void sst_mmio_register_range(const char *spec);

/* Reserve registered apertures as PROT_NONE; call once after guest_base setup. */
void sst_mmio_apply_reservation(void);

/* SIGSEGV hook: services an aperture fault and resumes (no return), else returns. */
void sst_mmio_handle_fault(CPUState *cpu, abi_ptr guest_addr, uintptr_t host_pc);

#endif
