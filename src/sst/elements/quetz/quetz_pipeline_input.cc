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

#include <inttypes.h>

using namespace SST;
using namespace SST::Quetz;

class DefaultPipelineInput : public PipelineInput {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        DefaultPipelineInput,
        "quetz",
        "DefaultPipelineInput",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Drain QuetzCoreBackend into the per-vCPU staging queue.",
        SST::Quetz::PipelineInput)

    DefaultPipelineInput(ComponentId_t id, Params& params)
        : PipelineInput(id, params),
          backend_(nullptr),
          core_id_(0),
          exec_latency_(nullptr),
          compute_latency_(nullptr)
    {}

    void configure(const QuetzCoreContext& ctx) override {
        backend_          = ctx.backend;
        core_id_          = ctx.core_id;
        exec_latency_     = ctx.exec_latency;
        compute_latency_  = ctx.compute_latency;
    }

    bool refill(std::queue<PipelineEvent>& q,
                uint32_t                   max_queue_len,
                SST::Output*               out) override {
        bool saw_exit = false;
        while (q.size() < max_queue_len) {
            QuetzCommand cmd;
            if (!backend_->readCommandNB(core_id_, &cmd))
                break;

            uint32_t stall = 0;
            if (cmd.insn_class < (uint32_t)QUETZ_INSN_CLASS_COUNT) {
                if (cmd.cmd == QUETZ_CMD_READ || cmd.cmd == QUETZ_CMD_WRITE)
                    stall = exec_latency_[cmd.insn_class];
                else if (cmd.cmd == QUETZ_CMD_NOP)
                    stall = compute_latency_[cmd.insn_class];
            }

            q.push({ cmd, stall });

            if (cmd.cmd == QUETZ_CMD_EXIT) {
                out->verbose(CALL_INFO, 1, 0,
                    "QuetzCore %" PRIu32 " received EXIT from plugin.\n",
                    core_id_);
                saw_exit = true;
                break;
            }
        }
        return saw_exit;
    }

private:
    QuetzCoreBackend* backend_;
    uint32_t          core_id_;
    const uint32_t*   exec_latency_;
    const uint32_t*   compute_latency_;
};
