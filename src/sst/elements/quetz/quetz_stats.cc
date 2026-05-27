// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "quetz_stats.h"
#include "quetzcore.h"

namespace SST {
namespace Quetz {

void QuetzCoreStats::registerAll(QuetzCore* comp, const char* sub_id) {
    read_reqs             = comp->registerStatistic<uint64_t>("read_requests",       sub_id);
    write_reqs            = comp->registerStatistic<uint64_t>("write_requests",      sub_id);
    mmio_read_reqs        = comp->registerStatistic<uint64_t>("mmio_read_requests",  sub_id);
    mmio_write_reqs       = comp->registerStatistic<uint64_t>("mmio_write_requests", sub_id);
    read_lat              = comp->registerStatistic<uint64_t>("read_latency",        sub_id);
    write_lat             = comp->registerStatistic<uint64_t>("write_latency",       sub_id);
    mmio_read_lat         = comp->registerStatistic<uint64_t>("mmio_read_latency",   sub_id);
    mmio_write_lat        = comp->registerStatistic<uint64_t>("mmio_write_latency",  sub_id);
    mmio_truncated_writes = comp->registerStatistic<uint64_t>("mmio_truncated_writes", sub_id);
    mmio_doorbell_flushes = comp->registerStatistic<uint64_t>("mmio_doorbell_flushes", sub_id);
    mmio_doorbell_flush_cycles =
        comp->registerStatistic<uint64_t>("mmio_doorbell_flush_cycles", sub_id);
    read_req_sizes        = comp->registerStatistic<uint64_t>("read_request_sizes",  sub_id);
    write_req_sizes       = comp->registerStatistic<uint64_t>("write_request_sizes", sub_id);
    split_reads           = comp->registerStatistic<uint64_t>("split_read_requests", sub_id);
    split_writes          = comp->registerStatistic<uint64_t>("split_write_requests",sub_id);
    noop_count            = comp->registerStatistic<uint64_t>("no_ops",              sub_id);
    insn_count            = comp->registerStatistic<uint64_t>("instruction_count",   sub_id);
    cycles                = comp->registerStatistic<uint64_t>("cycles",              sub_id);
    active_cycles         = comp->registerStatistic<uint64_t>("active_cycles",       sub_id);
    filtered_reads        = comp->registerStatistic<uint64_t>("filtered_reads",      sub_id);
    filtered_writes       = comp->registerStatistic<uint64_t>("filtered_writes",     sub_id);
    gpu_doorbell_writes   = comp->registerStatistic<uint64_t>("gpu_doorbell_writes", sub_id);
    gpu_status_polls      = comp->registerStatistic<uint64_t>("gpu_status_polls",    sub_id);
    gpu_other_reads       = comp->registerStatistic<uint64_t>("gpu_other_reads",     sub_id);
    gpu_other_writes      = comp->registerStatistic<uint64_t>("gpu_other_writes",    sub_id);
    stall_cycles          = comp->registerStatistic<uint64_t>("stall_cycles",         sub_id);
    compute_stall_cycles  = comp->registerStatistic<uint64_t>("compute_stall_cycles", sub_id);
    int_compute           = comp->registerStatistic<uint64_t>("int_compute",         sub_id);
    fp_compute            = comp->registerStatistic<uint64_t>("fp_compute",          sub_id);
    vec_compute           = comp->registerStatistic<uint64_t>("vec_compute",         sub_id);
    branch                = comp->registerStatistic<uint64_t>("branch",              sub_id);
}

} // namespace Quetz
} // namespace SST
