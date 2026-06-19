// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "plugin_state.h"

#include <cstring>

namespace SST {
namespace Quetz {

PluginSHMChild*     g_shmchild         = nullptr;
QuetzTunnel*        g_tunnel           = nullptr;
std::string         g_shmem_name;
bool                g_detailed         = false;
bool                g_system_mode      = false;
std::atomic<bool>   g_mem_seen[PLUGIN_MAX_VCPUS];
QuetzInsnClass      g_prev_cls[PLUGIN_MAX_VCPUS];
InsnClassifier*     g_insn_classifier  = nullptr;
MemAccessHandler*   g_mem_handler      = nullptr;
uint64_t            g_mmio_sync_base   = 0;
uint64_t            g_mmio_sync_size   = 0;
QuetzInsnClass      g_run_cls[PLUGIN_MAX_VCPUS];
uint32_t            g_run_count[PLUGIN_MAX_VCPUS]  = {0};
uint64_t            g_run_pc[PLUGIN_MAX_VCPUS];

void flush_run(unsigned vcpu)
{
    if (vcpu >= PLUGIN_MAX_VCPUS || g_run_count[vcpu] == 0)
        return;
    QuetzCommand cmd{};
    cmd.cmd        = QUETZ_CMD_COMPUTE_RUN;
    cmd.size       = g_run_count[vcpu];
    cmd.pc         = g_run_pc[vcpu];
    cmd.insn_class = static_cast<uint32_t>(g_run_cls[vcpu]);
    g_tunnel->writeMessage((size_t)vcpu, cmd);
    g_run_count[vcpu] = 0;
}

void accumulate_compute(unsigned vcpu, uint64_t pc, QuetzInsnClass cls)
{
    if (vcpu >= PLUGIN_MAX_VCPUS)
        return;
    if (g_run_count[vcpu] > 0 && g_run_cls[vcpu] != cls)
        flush_run(vcpu);                       // class changed: close the old run
    g_run_cls[vcpu] = cls;
    g_run_pc[vcpu]  = pc;
    if (++g_run_count[vcpu] >= QUETZ_COMPUTE_RUN_MAX)
        flush_run(vcpu);
}

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
