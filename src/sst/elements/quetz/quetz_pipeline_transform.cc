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

#include <cstring>

using namespace SST;
using namespace SST::Quetz;

class DefaultPipelineTransform : public PipelineTransform {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        DefaultPipelineTransform,
        "quetz",
        "DefaultPipelineTransform",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Head-of-line stall and NOP / MemOp classification.",
        SST::Quetz::PipelineTransform)

    DefaultPipelineTransform(ComponentId_t id, Params& params)
        : PipelineTransform(id, params),
          detailed_tracking_(false)
    {}

    void configure(const QuetzCoreContext& ctx) override {
        detailed_tracking_ = ctx.detailed_tracking;
    }

    Result process(PipelineEvent& ev,
                   QuetzCoreStats& stats,
                   MemOp&          op_out) override {
        QuetzCommand& cmd = ev.cmd;

        if (cmd.cmd == QUETZ_CMD_EXIT)
            return Result::HALT_EXIT;

        if (ev.remaining_stall > 0) {
            ev.remaining_stall--;
            if (cmd.cmd == QUETZ_CMD_NOP)
                stats.compute_stall_cycles->addData(1);
            else
                stats.stall_cycles->addData(1);
            return Result::STALL;
        }

        if (cmd.cmd == QUETZ_CMD_NOP) {
            stats.noop_count->addData(1);
            stats.insn_count->addData(1);
            if (detailed_tracking_) {
                switch (static_cast<QuetzInsnClass>(cmd.insn_class)) {
                case QUETZ_INSN_INT_COMPUTE: stats.int_compute->addData(1); break;
                case QUETZ_INSN_FP_COMPUTE:  stats.fp_compute->addData(1);  break;
                case QUETZ_INSN_VEC_COMPUTE: stats.vec_compute->addData(1); break;
                case QUETZ_INSN_BRANCH:      stats.branch->addData(1);      break;
                default: break;
                }
            }
            return Result::CONSUMED_NOOP;
        }

        op_out.is_read = (cmd.cmd == QUETZ_CMD_READ);
        op_out.addr    = cmd.addr;
        op_out.size    = cmd.size;
        op_out.pc      = cmd.pc;
        memcpy(op_out.data, cmd.data, sizeof(op_out.data));
        return Result::EMIT_MEMOP;
    }

private:
    bool detailed_tracking_;
};
