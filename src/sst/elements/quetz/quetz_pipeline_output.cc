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

#include "quetz_pipeline_api.h"

using namespace SST;
using namespace SST::Quetz;

class DefaultPipelineOutput : public PipelineOutput {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        DefaultPipelineOutput,
        "quetz",
        "DefaultPipelineOutput",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Issue MemOps to memHierarchy via MemRequestEmitter.",
        SST::Quetz::PipelineOutput)

    DefaultPipelineOutput(ComponentId_t id, Params& params)
        : PipelineOutput(id, params),
          emitter_(nullptr)
    {}

    void configure(const QuetzCoreContext& /*ctx*/,
                   MemRequestEmitter&      emitter) override {
        emitter_ = &emitter;
    }

    uint32_t slotsNeeded(const MemOp& op) const override {
        IssuePath path = op.is_mmio ? IssuePath::MMIO : IssuePath::CACHED;
        return emitter_->slotsNeeded(op.addr, op.size, path);
    }

    void issue(const MemOp& op) override {
        IssuePath path = op.is_mmio ? IssuePath::MMIO : IssuePath::CACHED;
        if (op.is_read)
            emitter_->issueRead(op.addr, op.size, op.pc, path);
        else
            emitter_->issueWrite(op.addr, op.size, op.pc, op.data, path);
    }

    uint32_t pendingCount() const override {
        return emitter_->pendingCount();
    }

    void setMemLink(SST::Interfaces::StandardMem* link) override {
        emitter_->setLink(link);
    }

    void setMmioLink(SST::Interfaces::StandardMem* link) override {
        emitter_->setMmioLink(link);
    }

    bool handleResponse(SST::Interfaces::StandardMem::Request* resp,
                        uint64_t& latency_out,
                        bool&     was_read_out,
                        bool&     was_mmio_out) override {
        return emitter_->handleResponse(resp, latency_out, was_read_out, was_mmio_out);
    }

private:
    MemRequestEmitter* emitter_;
};
