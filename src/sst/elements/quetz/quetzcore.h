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

#ifndef _H_SST_QUETZ_CORE
#define _H_SST_QUETZ_CORE

#include <sst/core/componentExtension.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>
#include <sst/core/params.h>
#include <sst/core/timeConverter.h>

#include <stdint.h>

#include <functional>

#include "quetz_core_backend.h"
#include "quetz_pipeline.h"
#include "quetz_pipeline_api.h"
#include "quetz_region_table.h"
#include "quetz_shmem.h"
#include "quetz_stats.h"

namespace SST {
namespace Quetz {

class QuetzCore : public SST::ComponentExtension {
    friend struct QuetzCoreStats;

public:
    QuetzCore(
        ComponentId_t         id,
        QuetzCoreBackend*     backend,
        uint32_t              coreID,
        SST::Output*          out,
        TimeConverter         tc,
        Params&               params,
        const MemRegionTable* region_table,
        uint32_t              maxPendTrans,
        uint32_t              maxIssuePerCycle,
        uint32_t              maxQueueLen,
        uint64_t              cacheLineSize,
        uint64_t              maxInsts,
        uint32_t              checkAddresses,
        bool                  detailedTracking,
        const uint32_t        execLatency[QUETZ_INSN_CLASS_COUNT],
        const uint32_t        computeLatency[QUETZ_INSN_CLASS_COUNT]);

    ~QuetzCore();

    void setMemLink(SST::Interfaces::StandardMem* link);
    void setMmioLink(SST::Interfaces::StandardMem* link);
    using MmioSyncCompleter = std::function<bool(SST::Interfaces::StandardMem::Request*)>;
    void setMmioSyncCompleter(MmioSyncCompleter fn);

    void tick();
    void handleMemResponse(SST::Interfaces::StandardMem::Request* resp);

    void recordMmioSyncRequest(bool is_read);
    void recordMmioDoorbellFlush();
    void recordMmioDoorbellFlushCycles(uint64_t cycles);

    bool     isCoreHalted()  const;
    uint32_t pendingCount()  const;

    void finishCore();

private:
    template <typename T>
    T* loadPipelineStage(const char* slot_name, const char* default_type) {
        SubComponentSlotInfo* slot = getSubComponentSlotInfo(slot_name);
        if (slot && slot->isPopulated(core_id_))
            return slot->create<T>(core_id_, ComponentInfo::INSERT_STATS);
        Params p;
        return loadAnonymousSubComponent<T>(
            default_type, slot_name, core_id_,
            ComponentInfo::INSERT_STATS, p);
    }

    SST::Output*           output_;
    uint32_t               core_id_;
    uint32_t               exec_latency_[QUETZ_INSN_CLASS_COUNT];
    uint32_t               compute_latency_[QUETZ_INSN_CLASS_COUNT];
    QuetzCoreContext       ctx_;
    QuetzCoreStats         stats_;

    PipelineInput*         pipeline_input_;
    PipelineFilter*        pipeline_filter_;
    PipelineTransform*     pipeline_transform_;
    PipelineOutput*        pipeline_output_;
    QuetzEventPipeline*    pipeline_;

    MmioSyncCompleter mmio_sync_completer_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_CORE
