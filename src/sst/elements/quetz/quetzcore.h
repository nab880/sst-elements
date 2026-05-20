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
 * quetzcore.h — per-vCPU event pump for the Quetz SST component.
 *
 * On every SST clock tick the owning QuetzCPU calls tick() on each
 * QuetzCore.  QuetzCore drains commands from its shared-memory buffer,
 * stages them in a local queue, and issues up to maxIssuePer Cycle memory
 * requests through its StandardMem interface.
 *
 * Design mirrors ariel/arielcore.h so that existing SST infrastructure
 * (statistics, memHierarchy wiring, etc.) can be reused without modification.
 */

#ifndef _H_SST_QUETZ_CORE
#define _H_SST_QUETZ_CORE

#include <sst/core/componentExtension.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>
#include <sst/core/params.h>
#include <sst/core/timeConverter.h>

#include <stdint.h>
#include <queue>
#include <vector>

#include "quetz_mem_issue.h"
#include "quetz_memmap.h"
#include "quetz_shmem.h"
#include "quetz_stats.h"

namespace SST {
namespace Quetz {

struct StagedCmd {
    QuetzCommand cmd;
    uint32_t    remaining_stall;
};

class QuetzCore : public SST::ComponentExtension {
    friend struct QuetzCoreStats;

public:
    QuetzCore(
        ComponentId_t                   id,
        QuetzTunnel*                    tunnel,
        uint32_t                        coreID,
        uint32_t                        maxPendTrans,
        SST::Output*                    out,
        uint32_t                        maxIssuePerCycle,
        uint32_t                        maxQueueLen,
        uint64_t                        cacheLineSize,
        TimeConverter                   tc,
        Params&                         params,
        const uint32_t                  execLatency[QUETZ_INSN_CLASS_COUNT],
        const uint32_t                  computeLatency[QUETZ_INSN_CLASS_COUNT],
        const std::vector<MemRegion>&   memmap,
        uint64_t                        maxInsts,
        uint32_t                        checkAddresses,
        bool                            detailedTracking);

    ~QuetzCore();

    void setMemLink(SST::Interfaces::StandardMem* link);

    void tick();
    void handleMemResponse(SST::Interfaces::StandardMem::Request* resp);

    bool     isCoreHalted()  const { return halted_;  }
    bool     isCoreStalled() const { return stalled_; }
    uint32_t pendingCount()  const { return emitter_.pendingCount(); }

    void finishCore();

private:
    void refillQueue();
    void processQueue();

    QuetzTunnel*                  tunnel_;
    SST::Output*                  output_;
    TimeConverter                 tc_;

    uint32_t core_id_;
    uint32_t max_pending_;
    uint32_t max_issue_per_cycle_;
    uint32_t max_queue_len_;

    uint64_t max_insts_;
    uint64_t inst_count_;
    bool     detailed_tracking_;

    bool halted_;
    bool stalled_;

    std::queue<StagedCmd> coreQ_;

    uint32_t exec_latency_[QUETZ_INSN_CLASS_COUNT];
    uint32_t compute_latency_[QUETZ_INSN_CLASS_COUNT];

    MemMap             memmap_;
    MemRequestEmitter  emitter_;
    QuetzCoreStats     stats_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_CORE
