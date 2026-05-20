// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "plugin_state.h"

#include "insn_classifier.h"

#include <cstring>

namespace SST {
namespace Quetz {

PluginSHMChild*     g_shmchild    = nullptr;
QuetzTunnel*        g_tunnel      = nullptr;
std::string         g_shmem_name;
bool                g_detailed    = false;
bool                g_system_mode = false;
std::atomic<bool>   g_mem_seen[PLUGIN_MAX_VCPUS];
QuetzInsnClass      g_prev_cls[PLUGIN_MAX_VCPUS];
QuetzISA            g_isa = QUETZ_ISA_GENERIC;
InsnClassifier*     g_insn_classifier = nullptr;

void write_cmd(unsigned vcpu, QuetzShmemCmd type,
               uint32_t size, uint64_t pc, uint64_t addr,
               QuetzInsnClass cls,
               const uint8_t* store_data)
{
    QuetzCommand cmd;
    cmd.cmd        = type;
    cmd.size       = size;
    cmd.pc         = pc;
    cmd.addr       = addr;
    cmd.insn_class = static_cast<uint32_t>(cls);
    cmd._pad       = 0;
    if (store_data) {
        uint32_t n = (size < sizeof(cmd.data)) ? size : (uint32_t)sizeof(cmd.data);
        memcpy(cmd.data, store_data, n);
        if (n < sizeof(cmd.data))
            memset(cmd.data + n, 0, sizeof(cmd.data) - n);
    } else {
        memset(cmd.data, 0, sizeof(cmd.data));
    }
    g_tunnel->writeMessage((size_t)vcpu, cmd);
}

} // namespace Quetz
} // namespace SST
