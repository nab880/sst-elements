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
 * quetz_pipeline_api.h — SubComponent APIs for QuetzEventPipeline stages.
 */

#ifndef _H_SST_QUETZ_PIPELINE_API
#define _H_SST_QUETZ_PIPELINE_API

#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>
#include <sst/core/subcomponent.h>

#include <queue>
#include <stdint.h>

#include "quetz_core_backend.h"
#include "quetz_mem_issue.h"
#include "quetz_region_table.h"
#include "quetz_shmem.h"
#include "quetz_stats.h"

namespace SST {
namespace Quetz {

struct PipelineEvent {
    QuetzCommand cmd;
    uint32_t     remaining_stall;
};

struct MemOp {
    bool     is_read;
    uint64_t addr;
    uint32_t size;
    uint64_t pc;
    uint8_t  data[sizeof(QuetzCommand::data)];
};

struct QuetzCoreContext {
    QuetzCoreBackend*        backend;
    SST::ComponentExtension* comp;
    SST::Output*             out;
    SST::TimeConverter       tc;
    uint32_t                 core_id;
    uint32_t                 max_queue_len;
    uint32_t                 max_issue_per_cycle;
    uint32_t                 max_pending;
    uint64_t                 max_insts;
    uint64_t                 cache_line_size;
    uint32_t                 check_addresses;
    bool                     detailed_tracking;
    const MemRegionTable*    region_table;
    const uint32_t*          exec_latency;
    const uint32_t*          compute_latency;
    QuetzCoreStats*          stats;
};

class PipelineInput : public SST::SubComponent {
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(SST::Quetz::PipelineInput)

    PipelineInput(ComponentId_t id, Params& params)
        : SST::SubComponent(id)
    {}

    virtual void configure(const QuetzCoreContext& ctx) = 0;

    virtual bool refill(std::queue<PipelineEvent>& q,
                        uint32_t                   max_queue_len,
                        SST::Output*               out) = 0;
};

class PipelineFilter : public SST::SubComponent {
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(SST::Quetz::PipelineFilter)

    PipelineFilter(ComponentId_t id, Params& params)
        : SST::SubComponent(id)
    {}

    virtual void configure(const QuetzCoreContext& ctx) = 0;

    virtual bool handle(const QuetzCommand& cmd, QuetzCoreStats& stats) = 0;
    virtual void finish(SST::Output* out, uint32_t core_id) = 0;
};

class PipelineTransform : public SST::SubComponent {
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(SST::Quetz::PipelineTransform)

    enum class Result {
        STALL,
        CONSUMED_NOOP,
        EMIT_MEMOP,
        HALT_EXIT,
    };

    PipelineTransform(ComponentId_t id, Params& params)
        : SST::SubComponent(id)
    {}

    virtual void configure(const QuetzCoreContext& ctx) = 0;

    virtual Result process(PipelineEvent& ev,
                           QuetzCoreStats& stats,
                           MemOp&          op_out) = 0;
};

class PipelineOutput : public SST::SubComponent {
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(SST::Quetz::PipelineOutput)

    PipelineOutput(ComponentId_t id, Params& params)
        : SST::SubComponent(id)
    {}

    virtual void configure(const QuetzCoreContext& ctx,
                           MemRequestEmitter&      emitter) = 0;

    virtual uint32_t slotsNeeded(const MemOp& op) const = 0;
    virtual void     issue(const MemOp& op) = 0;
    virtual uint32_t pendingCount() const = 0;

    virtual void setMemLink(SST::Interfaces::StandardMem* link) = 0;

    virtual bool handleResponse(SST::Interfaces::StandardMem::Request* resp,
                                uint64_t& latency_out,
                                bool&     was_read_out) = 0;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_PIPELINE_API
