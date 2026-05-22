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

#include "quetz_statistics_collector.h"
#include "quetz_stats.h"

namespace SST {
namespace Quetz {

void QuetzStatisticsCollector::recordCycle() {
    if (core_stats_ && core_stats_->cycles)
        core_stats_->cycles->addData(1);
}

void QuetzStatisticsCollector::recordActiveCycle() {
    if (core_stats_ && core_stats_->active_cycles)
        core_stats_->active_cycles->addData(1);
}

void QuetzStatisticsCollector::recordInsnCount() {
    if (core_stats_ && core_stats_->insn_count)
        core_stats_->insn_count->addData(1);
}

void QuetzStatisticsCollector::recordNoop() {
    if (core_stats_ && core_stats_->noop_count)
        core_stats_->noop_count->addData(1);
}

void QuetzStatisticsCollector::recordReadLatency(uint64_t lat) {
    if (core_stats_ && core_stats_->read_lat)
        core_stats_->read_lat->addData(lat);
}

void QuetzStatisticsCollector::recordWriteLatency(uint64_t lat) {
    if (core_stats_ && core_stats_->write_lat)
        core_stats_->write_lat->addData(lat);
}

void QuetzStatisticsCollector::recordStallCycle() {
    if (core_stats_ && core_stats_->stall_cycles)
        core_stats_->stall_cycles->addData(1);
}

void QuetzStatisticsCollector::recordComputeStallCycle() {
    if (core_stats_ && core_stats_->compute_stall_cycles)
        core_stats_->compute_stall_cycles->addData(1);
}

void QuetzStatisticsCollector::recordIntCompute() {
    if (core_stats_ && core_stats_->int_compute)
        core_stats_->int_compute->addData(1);
}

void QuetzStatisticsCollector::recordFpCompute() {
    if (core_stats_ && core_stats_->fp_compute)
        core_stats_->fp_compute->addData(1);
}

void QuetzStatisticsCollector::recordVecCompute() {
    if (core_stats_ && core_stats_->vec_compute)
        core_stats_->vec_compute->addData(1);
}

void QuetzStatisticsCollector::recordBranch() {
    if (core_stats_ && core_stats_->branch)
        core_stats_->branch->addData(1);
}

} // namespace Quetz
} // namespace SST
