// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2026, NTESS
// All rights reserved.

/**
 * quetz_logging_pipeline_output.cc — test/demo PipelineOutput that wraps
 * DefaultPipelineOutput and logs each issued MemOp to stderr.
 */

#include "quetz_pipeline_api.h"

#include <cinttypes>
#include <cstdio>

using namespace SST;
using namespace SST::Quetz;

class LoggingPipelineOutput : public PipelineOutput {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        LoggingPipelineOutput,
        "quetz",
        "LoggingPipelineOutput",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Default output stage with stderr logging (for pipeline slot tests).",
        SST::Quetz::PipelineOutput)

    LoggingPipelineOutput(ComponentId_t id, Params& params)
        : PipelineOutput(id, params),
          delegate_(nullptr),
          core_id_(0)
    {}

    void configure(const QuetzCoreContext& ctx,
                   MemRequestEmitter&      emitter) override {
        core_id_ = ctx.core_id;
        if (!delegate_) {
            Params p;
            delegate_ = loadAnonymousSubComponent<PipelineOutput>(
                "quetz.DefaultPipelineOutput", "pipeline_output_delegate",
                0, ComponentInfo::INSERT_STATS, p);
        }
        delegate_->configure(ctx, emitter);
    }

    uint32_t slotsNeeded(const MemOp& op) const override {
        return delegate_->slotsNeeded(op);
    }

    void issue(const MemOp& op) override {
        fprintf(stderr,
            "[LoggingPipelineOutput] core=%" PRIu32 " %s addr=0x%016" PRIx64
            " size=%" PRIu32 "\n",
            core_id_, op.is_read ? "READ" : "WRITE", op.addr, op.size);
        delegate_->issue(op);
    }

    uint32_t pendingCount() const override {
        return delegate_->pendingCount();
    }

    void setMemLink(SST::Interfaces::StandardMem* link) override {
        delegate_->setMemLink(link);
    }

    bool handleResponse(SST::Interfaces::StandardMem::Request* resp,
                        uint64_t& latency_out,
                        bool&     was_read_out) override {
        return delegate_->handleResponse(resp, latency_out, was_read_out);
    }

private:
    PipelineOutput* delegate_;
    uint32_t        core_id_;
};
