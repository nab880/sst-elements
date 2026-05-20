// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include "quetz_config.h"

#include <cstring>
#include <inttypes.h>
#include <sstream>

using namespace SST;
using namespace SST::Quetz;

QuetzConfig QuetzConfig::fromParams(Params& params, SST::Output* out) {
    QuetzConfig cfg;

    cfg.verbosity = params.find<int>("verbose", 0);

    cfg.vcpu_count     = params.find<uint32_t>("vcpu_count", 1);
    cfg.max_core_queue = params.find<uint32_t>("maxcorequeue", 64);
    cfg.max_pend_trans = params.find<uint32_t>("maxtranscore", 16);
    cfg.max_issue_cyc  = params.find<uint32_t>("maxissuepercycle", 1);
    cfg.cache_line_sz  = params.find<uint64_t>("cachelinesize", 64);
    cfg.cpu_clock      = params.find<std::string>("clock", "1GHz");

    cfg.system_mode        = params.find<bool>("system_mode", false);
    cfg.system_mode_loader = params.find<std::string>("system_mode_loader", "-kernel");
    cfg.qemu_bin           = params.find<std::string>("qemu", "qemu-riscv64");
    cfg.qemu_plugin        = params.find<std::string>("qemu_plugin", "");
    cfg.executable         = params.find<std::string>("executable", "");

    if (cfg.executable.empty())
        out->fatal(CALL_INFO, -1,
            "No 'executable' parameter provided — nothing to run.\n");

    cfg.appargcount = params.find<uint32_t>("appargcount", 0);
    {
        char buf[256];
        for (uint32_t i = 0; i < cfg.appargcount; i++) {
            snprintf(buf, sizeof(buf), "apparg%" PRIu32, i);
            cfg.app_args.push_back(params.find<std::string>(buf, ""));
        }
    }

    cfg.stdin_file  = params.find<std::string>("appstdin", "");
    cfg.stdout_file = params.find<std::string>("appstdout", "");
    cfg.stderr_file = params.find<std::string>("appstderr", "");
    cfg.max_insts         = params.find<uint64_t>("max_insts", 0ULL);
    cfg.check_addresses   = params.find<uint32_t>("checkaddresses", 0);
    cfg.detailed_tracking = params.find<bool>("detailed_instruction_tracking", false);

    std::string qemu_args_str = params.find<std::string>("qemu_args", "");
    if (!qemu_args_str.empty()) {
        std::istringstream iss(qemu_args_str);
        std::string tok;
        while (iss >> tok)
            cfg.qemu_extra_args.push_back(tok);
    }

    cfg.isa_str     = params.find<std::string>("isa", "");
    cfg.has_fpu     = params.find<bool>("has_fpu", false);
    cfg.has_vector  = params.find<bool>("has_vector", false);
    cfg.vector_vlen = params.find<uint32_t>("vector_vlen", 128);
    cfg.vector_elen = params.find<uint32_t>("vector_elen", 64);

    if (!cfg.isa_str.empty())
        out->verbose(CALL_INFO, 1, 0,
            "Modeled ISA: %s  fpu=%d  vector=%d  vlen=%" PRIu32
            "  elen=%" PRIu32 "\n",
            cfg.isa_str.c_str(), (int)cfg.has_fpu, (int)cfg.has_vector,
            cfg.vector_vlen, cfg.vector_elen);

    memset(cfg.exec_latency, 0, sizeof(cfg.exec_latency));
    cfg.exec_latency[QUETZ_INSN_INT_MEM] = params.find<uint32_t>("exec_latency_int", 0);
    cfg.exec_latency[QUETZ_INSN_FP_MEM]  = params.find<uint32_t>("exec_latency_fp", 0);
    cfg.exec_latency[QUETZ_INSN_VEC_MEM] = params.find<uint32_t>("exec_latency_vec", 0);

    memset(cfg.compute_latency, 0, sizeof(cfg.compute_latency));
    cfg.compute_latency[QUETZ_INSN_INT_COMPUTE] = params.find<uint32_t>("compute_latency_int", 0);
    cfg.compute_latency[QUETZ_INSN_FP_COMPUTE]  = params.find<uint32_t>("compute_latency_fp", 0);
    cfg.compute_latency[QUETZ_INSN_VEC_COMPUTE] = params.find<uint32_t>("compute_latency_vec", 0);
    cfg.compute_latency[QUETZ_INSN_BRANCH]      = params.find<uint32_t>("compute_latency_branch", 0);
    cfg.compute_latency[QUETZ_INSN_OTHER]       = params.find<uint32_t>("compute_latency_other", 0);

    {
        bool needs_detailed =
            cfg.compute_latency[QUETZ_INSN_INT_COMPUTE] ||
            cfg.compute_latency[QUETZ_INSN_FP_COMPUTE]  ||
            cfg.compute_latency[QUETZ_INSN_VEC_COMPUTE] ||
            cfg.compute_latency[QUETZ_INSN_BRANCH];
        if (needs_detailed && !cfg.detailed_tracking)
            out->fatal(CALL_INFO, -1,
                "compute_latency_int/fp/vec/branch require "
                "detailed_instruction_tracking=1.  Without it the plugin "
                "cannot distinguish instruction classes and all non-memory "
                "instructions arrive as OTHER.  Either set "
                "detailed_instruction_tracking=1 or use "
                "compute_latency_other for a class-independent delay.\n");
    }

    uint32_t mmcount = params.find<uint32_t>("memmap_count", 0);
    {
        char buf[256];
        for (uint32_t r = 0; r < mmcount; r++) {
            MemRegion region;
            snprintf(buf, sizeof(buf), "memmap%" PRIu32 "_name", r);
            region.name = params.find<std::string>(buf, "");
            snprintf(buf, sizeof(buf), "memmap%" PRIu32 "_start", r);
            region.start = params.find<uint64_t>(buf, 0ULL);
            snprintf(buf, sizeof(buf), "memmap%" PRIu32 "_end", r);
            region.end = params.find<uint64_t>(buf, 0ULL);
            snprintf(buf, sizeof(buf), "memmap%" PRIu32 "_type", r);
            std::string type = params.find<std::string>(buf, "memory");
            if (type == "filtered")
                region.type = MemRegionType::FILTERED;
            else if (type == "uart")
                region.type = MemRegionType::UART;
            else
                region.type = MemRegionType::MEMORY;

            snprintf(buf, sizeof(buf), "memmap%" PRIu32 "_uart_tx_offset", r);
            region.uart_tx_offset = params.find<uint32_t>(buf, 0);

            const char* type_str =
                (region.type == MemRegionType::FILTERED) ? "filtered" :
                (region.type == MemRegionType::UART)     ? "uart"     : "memory";
            out->verbose(CALL_INFO, 1, 0,
                "MemMap region[%" PRIu32 "] '%s': "
                "0x%016" PRIx64 "-0x%016" PRIx64 " (%s)\n",
                r, region.name.c_str(), region.start, region.end, type_str);
            cfg.memmap.push_back(region);
        }
    }

    int32_t env_count = params.find<int32_t>("envparamcount", -1);
    if (env_count > 0) {
        char buf[256];
        for (int32_t i = 0; i < env_count; i++) {
            snprintf(buf, sizeof(buf), "envparamname%" PRId32, i);
            std::string name = params.find<std::string>(buf, "");
            snprintf(buf, sizeof(buf), "envparamval%" PRId32, i);
            std::string val = params.find<std::string>(buf, "");
            cfg.extra_env.emplace_back(name, val);
        }
    }

    return cfg;
}
