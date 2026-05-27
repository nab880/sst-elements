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

#ifndef _H_SST_QUETZ_CONFIG
#define _H_SST_QUETZ_CONFIG

#include <sst/core/output.h>
#include <sst/core/params.h>

#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

#include "quetz_shmem.h"

namespace SST {
namespace Quetz {

/** Platform-preset or programmatic default for a region_handler slot. */
struct RegionHandlerPreset {
    std::string type;
    std::vector<std::pair<std::string, std::string>> params;
};

struct QuetzConfig {
    int verbosity = 0;

    uint32_t vcpu_count       = 1;
    uint32_t max_core_queue   = 64;
    uint32_t max_pend_trans   = 16;
    uint32_t max_issue_cyc    = 1;
    uint64_t cache_line_sz    = 64;
    std::string cpu_clock     = "1GHz";

    uint64_t balar_doorbell_addr       = 0;
    uint64_t balar_doorbell_size       = 8;
    uint64_t balar_packet_flush_bytes  = 4096;

    bool        system_mode         = false;
    std::string system_mode_loader  = "-kernel";
    std::string qemu_bin            = "qemu-riscv64";
    std::string qemu_plugin;
    std::string executable;

    uint32_t appargcount = 0;
    std::vector<std::string> app_args;
    std::vector<std::string> qemu_extra_args;

    std::string stdin_file;
    std::string stdout_file;
    std::string stderr_file;

    uint64_t max_insts         = 0;
    uint32_t check_addresses   = 0;
    bool     detailed_tracking = false;

    std::string isa_str;
    bool        has_fpu    = false;
    bool        has_vector = false;
    uint32_t    vector_vlen = 128;
    uint32_t    vector_elen = 64;

    uint32_t exec_latency[QUETZ_INSN_CLASS_COUNT];
    uint32_t compute_latency[QUETZ_INSN_CLASS_COUNT];

    std::vector<RegionHandlerPreset>                 region_handlers;
    std::vector<std::pair<std::string, std::string>> extra_env;

    static QuetzConfig fromParams(Params& params, SST::Output* out);
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_CONFIG
