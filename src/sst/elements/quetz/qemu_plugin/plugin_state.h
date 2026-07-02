// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef _QUETZ_PLUGIN_STATE_H
#define _QUETZ_PLUGIN_STATE_H

#include "../quetz_shmem.h"
#include <sst/core/interprocess/shmchild.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace SST {
namespace Quetz {

constexpr unsigned PLUGIN_MAX_VCPUS = 256;

using PluginSHMChild = SST::Core::Interprocess::SHMChild<QuetzTunnel>;

class InsnClassifier;
class MemAccessHandler;

extern PluginSHMChild*     g_shmchild;
extern QuetzTunnel*         g_tunnel;
extern std::string          g_shmem_name;
extern bool                 g_detailed;
extern bool                 g_system_mode;
extern std::atomic<bool>    g_mem_seen[PLUGIN_MAX_VCPUS];
extern QuetzInsnClass       g_prev_cls[PLUGIN_MAX_VCPUS];
extern InsnClassifier*      g_insn_classifier;
extern MemAccessHandler*    g_mem_handler;

extern uint64_t             g_mmio_sync_base;
extern uint64_t             g_mmio_sync_size;

// SST-backed memory window (win_base=/win_size=): like the MMIO range, guest
// accesses here are delivered synchronously via the sst-mmio-bridge device, so
// they must NOT also be emitted on the trace ring.
extern uint64_t             g_sst_win_base;
extern uint64_t             g_sst_win_size;

// Per-vCPU compute-run accumulator (P2): consecutive same-class non-memory
// insns are batched into one QUETZ_CMD_COMPUTE_RUN instead of one NOP each.
extern QuetzInsnClass       g_run_cls[PLUGIN_MAX_VCPUS];
extern uint32_t             g_run_count[PLUGIN_MAX_VCPUS];
extern uint64_t             g_run_pc[PLUGIN_MAX_VCPUS];

void write_cmd(unsigned vcpu, QuetzShmemCmd type,
               uint32_t size, uint64_t pc, uint64_t addr,
               QuetzInsnClass cls = QUETZ_INSN_OTHER,
               const uint8_t* store_data = nullptr);

// Batch a non-memory insn into the vCPU's run; flushes on class change or cap.
void accumulate_compute(unsigned vcpu, uint64_t pc, QuetzInsnClass cls);

// Emit the vCPU's pending run (if any) to the ring. Call before any memory /
// MMIO / EXIT command so program order on the ring is preserved.
void flush_run(unsigned vcpu);

} // namespace Quetz
} // namespace SST

#endif // _QUETZ_PLUGIN_STATE_H
