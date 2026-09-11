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
#include "insn_classifier.h"
#include "mem_access_handler.h"
#include "plugin_state.h"
#include "registry.h"

#include <cstdio>
#include <cstring>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

using namespace SST::Quetz;

static void cb_atexit(qemu_plugin_id_t , void* )
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

    for (int v = 0; v < num_vcpus; v++) {
        require_vcpu((unsigned)v);
        flush_run((unsigned)v);    // drain the tail compute run before EXIT
        g_tunnel->writeMessage((size_t)v, exit_cmd);
    }
}

extern "C"
QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id,
                        const qemu_info_t* info,
                        int argc, char** argv)
{
    const char* target = (info && info->target_name) ? info->target_name : "";
    if (info)
        g_system_mode = info->system_emulation;

    g_insn_classifier = Registry<InsnClassifier>::instance().findByPrefix(target);
    g_mem_handler     = Registry<MemAccessHandler>::instance().findByPrefix(target);

    if (!g_insn_classifier || !g_mem_handler) {
        fprintf(stderr,
            "[qemu_sst_plugin] ERROR: no classifier/handler for target '%s'.\n",
            target);
        return 1;
    }

    fprintf(stderr,
        "[qemu_sst_plugin] Target: %s  precise_mem=%d  system_mode: %d\n",
        target, (int)g_insn_classifier->usesPreciseMemCallbacks(),
        (int)g_system_mode);

    for (unsigned i = 0; i < PLUGIN_MAX_VCPUS; i++) {
        g_mem_seen[i].store(false, std::memory_order_relaxed);
        g_prev_cls[i] = QUETZ_INSN_OTHER;
        g_run_count[i] = 0;
    }

    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "shmname=", 8) == 0)
            g_shmem_name = std::string(argv[i] + 8);
        else if (strcmp(argv[i], "cache_ops=1") == 0)
            g_cache_ops = true;
        else if (strncmp(argv[i], "detailed=", 9) == 0)
            g_detailed = (argv[i][9] == '1');
        else if (strncmp(argv[i], "mmio_base=", 10) == 0)
            g_mmio_sync_base = strtoull(argv[i] + 10, nullptr, 0);
        else if (strncmp(argv[i], "mmio_size=", 10) == 0)
            g_mmio_sync_size = strtoull(argv[i] + 10, nullptr, 0);
        else if (strncmp(argv[i], "win_base=", 9) == 0)
            g_sst_win_base = strtoull(argv[i] + 9, nullptr, 0);
        else if (strncmp(argv[i], "win_size=", 9) == 0)
            g_sst_win_size = strtoull(argv[i] + 9, nullptr, 0);
    }

    if (g_cache_ops && (strcmp(target, "m68k") != 0 || !g_system_mode ||
                        g_sst_win_size == 0)) {
        fprintf(stderr, "[qemu_sst_plugin] ERROR: cache_ops=1 requires "
                        "ColdFire system mode and an SST-backed window.\n");
        return 1;
    }
#if QEMU_PLUGIN_VERSION < 3
    if (g_cache_ops) {
        fprintf(stderr, "[qemu_sst_plugin] ERROR: cache_ops=1 requires "
                        "QEMU plugin API version 3 or newer.\n");
        return 1;
    }
#endif

    if (g_detailed && !g_insn_classifier->usesPreciseMemCallbacks()) {
        fprintf(stderr,
            "[qemu_sst_plugin] WARNING: detailed instruction tracking "
            "requested but ISA '%s' has no instruction decoder.\n"
            "  All non-memory instructions will be reported as OTHER.\n"
            "  Register a decoder via QUETZ_REGISTER_CLASSIFIER.\n",
            target);
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

    g_configured_vcpus = g_tunnel->getSharedData()->numCores;
    if (!g_configured_vcpus || g_configured_vcpus > PLUGIN_MAX_VCPUS ||
        (g_system_mode && info->system.smp_vcpus != (int)g_configured_vcpus)) {
        fprintf(stderr, "[qemu_sst_plugin] ERROR: configured trace-ring count %u "
                        "does not match the QEMU vCPU topology.\n", g_configured_vcpus);
        return 1;
    }

    register_plugin_callbacks(id);
    qemu_plugin_register_atexit_cb(id, cb_atexit, nullptr);
    g_tunnel->sync().announceAttach();

    fprintf(stderr,
        "[qemu_sst_plugin] Attached to SST via shmem region '%s'%s.\n",
        g_shmem_name.c_str(),
        g_detailed ? " (detailed instruction tracking)" : "");
    return 0;
}
