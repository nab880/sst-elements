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

#include "quetz_pipeline.h"

#include <inttypes.h>

using namespace SST;
using namespace SST::Quetz;

QuetzEventPipeline::QuetzEventPipeline(QuetzCoreContext& ctx,
                                       PipelineInput*    input,
                                       PipelineFilter*   filter,
                                       PipelineTransform* transform,
                                       PipelineOutput*   output)
    : ctx_(ctx),
      input_(input),
      filter_(filter),
      transform_(transform),
      output_(output),
      emitter_(ctx.comp, ctx.out, ctx.core_id, ctx.tc,
               ctx.cache_line_size, ctx.check_addresses, *ctx.stats),
      inst_count_(0),
      halted_(false)
{
    output_->configure(ctx_, emitter_);
    ctx.out->verbose(CALL_INFO, 1, 0,
        "QuetzEventPipeline %" PRIu32 " created: maxQ=%" PRIu32
        " maxIssue=%" PRIu32 " maxPend=%" PRIu32 "\n",
        ctx.core_id, ctx.max_queue_len, ctx.max_issue_per_cycle,
        ctx.max_pending);
}

bool QuetzEventPipeline::checkMaxInsts() {
    if (ctx_.max_insts > 0 && inst_count_ >= ctx_.max_insts) {
        ctx_.out->verbose(CALL_INFO, 1, 0,
            "QuetzCore %" PRIu32 " reached max_insts %" PRIu64 " — halting.\n",
            ctx_.core_id, ctx_.max_insts);
        halted_ = true;
        return true;
    }
    return false;
}

void QuetzEventPipeline::tick() {
    if (halted_)
        return;

    input_->refill(coreQ_, ctx_.max_queue_len, ctx_.out);

    uint32_t issued = 0;
    MemOp    op;

    while (!coreQ_.empty()) {
        PipelineEvent& ev = coreQ_.front();

        MemRegionHandler::Action region_action = MemRegionHandler::Action::FORWARD;
        if ((ev.cmd.cmd == QUETZ_CMD_READ || ev.cmd.cmd == QUETZ_CMD_WRITE) &&
            filter_->handle(ev.cmd, *ctx_.stats, region_action))
        {
            ctx_.stats->insn_count->addData(1);
            coreQ_.pop();
            inst_count_++;
            if (checkMaxInsts()) return;
            continue;
        }

        switch (transform_->process(ev, *ctx_.stats, op, region_action)) {
        case PipelineTransform::Result::HALT_EXIT:
            ctx_.out->verbose(CALL_INFO, 1, 0,
                "QuetzCore %" PRIu32 " processing EXIT — halting.\n",
                ctx_.core_id);
            halted_ = true;
            coreQ_.pop();
            return;

        case PipelineTransform::Result::STALL:
            goto done;

        case PipelineTransform::Result::CONSUMED_NOOP:
            coreQ_.pop();
            inst_count_++;
            if (checkMaxInsts()) return;
            continue;

        case PipelineTransform::Result::EMIT_MEMOP: {
            uint32_t slots = output_->slotsNeeded(op);
            if (issued + slots > ctx_.max_issue_per_cycle) goto done;
            if (output_->pendingCount() + slots > ctx_.max_pending) goto done;

            output_->issue(op);
            ctx_.stats->insn_count->addData(1);
            issued += slots;
            coreQ_.pop();
            inst_count_++;
            if (checkMaxInsts()) return;
            break;
        }
        }
    }

done:
    if (issued > 0)
        ctx_.stats->active_cycles->addData(1);
    ctx_.stats->cycles->addData(1);
}

void QuetzEventPipeline::finish() {
    ctx_.out->verbose(CALL_INFO, 1, 0,
        "QuetzEventPipeline %" PRIu32 " finishing, %" PRIu32
        " transactions still pending.\n",
        ctx_.core_id, output_->pendingCount());
    filter_->finish(ctx_.out, ctx_.core_id);
}
