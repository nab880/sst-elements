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

/**
 * quetz_statistics_collector.h — SHM performance metrics and SST stats facade.
 *
 * SHM methods are inline (plugin-safe). record* helpers are implemented in
 * quetz_statistics_collector.cc (libquetz only).
 */

#ifndef _SST_QUETZ_STATISTICS_COLLECTOR_H
#define _SST_QUETZ_STATISTICS_COLLECTOR_H

#include "quetz_ipc_types.h"

namespace SST {
namespace Quetz {

struct QuetzCoreStats;

class QuetzStatisticsCollector {
public:
    void bindShared(QuetzSharedData* shared) { shared_ = shared; }
    void bindCoreStats(QuetzCoreStats* stats) { core_stats_ = stats; }

    void initMaster() {
        shared_->simTime   = 0;
        shared_->simCycles = 0;
    }

    void updateSimTime(uint64_t ns) { shared_->simTime = ns; }

    void incrementSimCycles() { shared_->simCycles++; }

    uint64_t getSimCycles() const { return shared_->simCycles; }

    void recordCycle();
    void recordActiveCycle();
    void recordInsnCount();
    void recordNoop();
    void recordReadLatency(uint64_t lat);
    void recordWriteLatency(uint64_t lat);
    void recordStallCycle();
    void recordComputeStallCycle();
    void recordIntCompute();
    void recordFpCompute();
    void recordVecCompute();
    void recordBranch();

private:
    QuetzSharedData* shared_     = nullptr;
    QuetzCoreStats*  core_stats_ = nullptr;
};

} // namespace Quetz
} // namespace SST

#endif // _SST_QUETZ_STATISTICS_COLLECTOR_H
