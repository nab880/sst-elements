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

enum QuetzISA { QUETZ_ISA_RISCV, QUETZ_ISA_AARCH64, QUETZ_ISA_GENERIC };

constexpr unsigned PLUGIN_MAX_VCPUS = 256;

using PluginSHMChild = SST::Core::Interprocess::SHMChild<QuetzTunnel>;

extern PluginSHMChild*     g_shmchild;
extern QuetzTunnel*         g_tunnel;
extern std::string          g_shmem_name;
extern bool                 g_detailed;
extern bool                 g_system_mode;
extern std::atomic<bool>    g_mem_seen[PLUGIN_MAX_VCPUS];
extern QuetzInsnClass       g_prev_cls[PLUGIN_MAX_VCPUS];
extern QuetzISA             g_isa;

class InsnClassifier;
extern InsnClassifier*      g_insn_classifier;

void write_cmd(unsigned vcpu, QuetzShmemCmd type,
               uint32_t size, uint64_t pc, uint64_t addr,
               QuetzInsnClass cls = QUETZ_INSN_OTHER,
               const uint8_t* store_data = nullptr);

} // namespace Quetz
} // namespace SST

#endif // _QUETZ_PLUGIN_STATE_H
