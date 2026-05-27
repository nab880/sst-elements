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

#ifndef _H_SST_QUETZ_STATS
#define _H_SST_QUETZ_STATS

#include <sst/core/statapi/statbase.h>

namespace SST {
namespace Quetz {

class QuetzCore;

struct QuetzCoreStats {
    Statistics::Statistic<uint64_t>* read_reqs        = nullptr;
    Statistics::Statistic<uint64_t>* write_reqs       = nullptr;
    Statistics::Statistic<uint64_t>* mmio_read_reqs   = nullptr;
    Statistics::Statistic<uint64_t>* mmio_write_reqs  = nullptr;
    Statistics::Statistic<uint64_t>* read_lat         = nullptr;
    Statistics::Statistic<uint64_t>* write_lat        = nullptr;
    Statistics::Statistic<uint64_t>* mmio_read_lat    = nullptr;
    Statistics::Statistic<uint64_t>* mmio_write_lat   = nullptr;
    Statistics::Statistic<uint64_t>* mmio_truncated_writes = nullptr;
    Statistics::Statistic<uint64_t>* mmio_doorbell_flushes = nullptr;
    Statistics::Statistic<uint64_t>* mmio_doorbell_flush_cycles = nullptr;
    Statistics::Statistic<uint64_t>* read_req_sizes   = nullptr;
    Statistics::Statistic<uint64_t>* write_req_sizes  = nullptr;
    Statistics::Statistic<uint64_t>* split_reads      = nullptr;
    Statistics::Statistic<uint64_t>* split_writes     = nullptr;
    Statistics::Statistic<uint64_t>* noop_count       = nullptr;
    Statistics::Statistic<uint64_t>* insn_count       = nullptr;
    Statistics::Statistic<uint64_t>* cycles           = nullptr;
    Statistics::Statistic<uint64_t>* active_cycles    = nullptr;
    Statistics::Statistic<uint64_t>* filtered_reads   = nullptr;
    Statistics::Statistic<uint64_t>* filtered_writes  = nullptr;
    Statistics::Statistic<uint64_t>* gpu_doorbell_writes = nullptr;
    Statistics::Statistic<uint64_t>* gpu_status_polls    = nullptr;
    Statistics::Statistic<uint64_t>* gpu_other_reads     = nullptr;
    Statistics::Statistic<uint64_t>* gpu_other_writes    = nullptr;
    Statistics::Statistic<uint64_t>* stall_cycles         = nullptr;
    Statistics::Statistic<uint64_t>* compute_stall_cycles = nullptr;
    Statistics::Statistic<uint64_t>* int_compute      = nullptr;
    Statistics::Statistic<uint64_t>* fp_compute       = nullptr;
    Statistics::Statistic<uint64_t>* vec_compute      = nullptr;
    Statistics::Statistic<uint64_t>* branch           = nullptr;

    void registerAll(QuetzCore* comp, const char* sub_id);
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_STATS
