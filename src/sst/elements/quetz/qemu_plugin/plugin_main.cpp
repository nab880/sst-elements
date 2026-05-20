// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include <glib.h>
extern "C" {
#include "qemu-plugin.h"
}

#include "instrument.h"
#include "plugin_state.h"

#include <cstdio>
#include <cstring>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

using namespace SST::Quetz;

static void cb_atexit(qemu_plugin_id_t /*id*/, void* /*userdata*/)
{
    if (!g_tunnel) return;

#if QEMU_PLUGIN_VERSION >= 2
    int num_vcpus = qemu_plugin_num_vcpus();
#else
    int num_vcpus = qemu_plugin_n_vcpus();
#endif
    if (num_vcpus <= 0) num_vcpus = 1;

    QuetzCommand exit_cmd{};
    exit_cmd.cmd = QUETZ_CMD_EXIT;

    for (int v = 0; v < num_vcpus && v < (int)PLUGIN_MAX_VCPUS; v++)
        g_tunnel->writeMessage((size_t)v, exit_cmd);
}

extern "C"
QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id,
                        const qemu_info_t* info,
                        int argc, char** argv)
{
    if (info && info->target_name) {
        const char* t = info->target_name;
        if      (strncmp(t, "riscv",   5) == 0) g_isa = QUETZ_ISA_RISCV;
        else if (strncmp(t, "aarch64", 7) == 0) g_isa = QUETZ_ISA_AARCH64;
        else                                     g_isa = QUETZ_ISA_GENERIC;
        g_system_mode = info->system_emulation;
        fprintf(stderr,
            "[qemu_sst_plugin] Target: %s  ISA class: %s  system_mode: %d\n", t,
            (g_isa == QUETZ_ISA_RISCV)   ? "riscv" :
            (g_isa == QUETZ_ISA_AARCH64) ? "aarch64" : "generic",
            (int)g_system_mode);
    }

    for (unsigned i = 0; i < PLUGIN_MAX_VCPUS; i++) {
        g_mem_seen[i].store(false, std::memory_order_relaxed);
        g_prev_cls[i] = QUETZ_INSN_OTHER;
    }

    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "shmname=", 8) == 0)
            g_shmem_name = std::string(argv[i] + 8);
        else if (strncmp(argv[i], "detailed=", 9) == 0)
            g_detailed = (argv[i][9] == '1');
    }

    if (g_detailed && g_isa == QUETZ_ISA_GENERIC) {
        fprintf(stderr,
            "[qemu_sst_plugin] WARNING: detailed instruction tracking "
            "requested but ISA '%s' has no instruction decoder.\n"
            "  All non-memory instructions will be reported as OTHER.\n"
            "  Add decoder_<isa>.h and a case in instrument.cpp.\n",
            (info && info->target_name) ? info->target_name : "unknown");
    }

    if (g_shmem_name.empty()) {
        fprintf(stderr,
            "[qemu_sst_plugin] ERROR: no 'shmname=<name>' argument.\n");
        return 1;
    }

    g_shmchild = new PluginSHMChild(g_shmem_name);
    g_tunnel   = g_shmchild->getTunnel();

    if (!g_tunnel) {
        fprintf(stderr,
            "[qemu_sst_plugin] ERROR: failed to attach to shmem '%s'.\n",
            g_shmem_name.c_str());
        return 1;
    }

    register_plugin_callbacks(id);
    qemu_plugin_register_atexit_cb(id, cb_atexit, nullptr);

    fprintf(stderr,
        "[qemu_sst_plugin] Attached to SST via shmem region '%s'%s.\n",
        g_shmem_name.c_str(),
        g_detailed ? " (detailed instruction tracking)" : "");
    return 0;
}
