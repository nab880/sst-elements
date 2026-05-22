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

#include <sst_config.h>
#include "quetzcore.h"

#include <inttypes.h>
#include <cstring>

using namespace SST;
using namespace SST::Quetz;

QuetzCore::QuetzCore(
        ComponentId_t         id,
        QuetzCoreBackend*     backend,
        uint32_t              coreID,
        SST::Output*          out,
        TimeConverter         tc,
        Params&               /*params*/,
        const MemRegionTable* region_table,
        uint32_t              maxPendTrans,
        uint32_t              maxIssuePerCycle,
        uint32_t              maxQueueLen,
        uint64_t              cacheLineSize,
        uint64_t              maxInsts,
        uint32_t              checkAddresses,
        bool                  detailedTracking,
        const uint32_t        execLatency[QUETZ_INSN_CLASS_COUNT],
        const uint32_t        computeLatency[QUETZ_INSN_CLASS_COUNT])
    : ComponentExtension(id),
      output_(out),
      core_id_(coreID),
      pipeline_input_(nullptr),
      pipeline_filter_(nullptr),
      pipeline_transform_(nullptr),
      pipeline_output_(nullptr),
      pipeline_(nullptr)
{
    memcpy(exec_latency_,    execLatency,    sizeof(exec_latency_));
    memcpy(compute_latency_, computeLatency, sizeof(compute_latency_));

    char sub_id[32];
    snprintf(sub_id, sizeof(sub_id), "%" PRIu32, coreID);
    stats_.registerAll(this, sub_id);

    ctx_.backend             = backend;
    ctx_.comp                = this;
    ctx_.out                 = out;
    ctx_.tc                  = tc;
    ctx_.core_id             = coreID;
    ctx_.max_queue_len       = maxQueueLen;
    ctx_.max_issue_per_cycle = maxIssuePerCycle;
    ctx_.max_pending         = maxPendTrans;
    ctx_.max_insts           = maxInsts;
    ctx_.cache_line_size     = cacheLineSize;
    ctx_.check_addresses     = checkAddresses;
    ctx_.detailed_tracking   = detailedTracking;
    ctx_.region_table        = region_table;
    ctx_.exec_latency        = exec_latency_;
    ctx_.compute_latency     = compute_latency_;
    ctx_.stats               = &stats_;

    pipeline_input_ = loadPipelineStage<PipelineInput>(
        "pipeline_input", "quetz.DefaultPipelineInput");
    pipeline_filter_ = loadPipelineStage<PipelineFilter>(
        "pipeline_filter", "quetz.DefaultPipelineFilter");
    pipeline_transform_ = loadPipelineStage<PipelineTransform>(
        "pipeline_transform", "quetz.DefaultPipelineTransform");
    pipeline_output_ = loadPipelineStage<PipelineOutput>(
        "pipeline_output", "quetz.DefaultPipelineOutput");

    pipeline_input_->configure(ctx_);
    pipeline_filter_->configure(ctx_);
    pipeline_transform_->configure(ctx_);

    pipeline_ = new QuetzEventPipeline(
        ctx_, pipeline_input_, pipeline_filter_,
        pipeline_transform_, pipeline_output_);

    output_->verbose(CALL_INFO, 1, 0,
        "QuetzCore %" PRIu32 " created: maxPend=%" PRIu32
        " maxIssue=%" PRIu32 " maxQ=%" PRIu32 " clineSize=%" PRIu64
        " latency[int/fp/vec]=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n",
        core_id_, maxPendTrans, maxIssuePerCycle, maxQueueLen, cacheLineSize,
        exec_latency_[QUETZ_INSN_INT_MEM],
        exec_latency_[QUETZ_INSN_FP_MEM],
        exec_latency_[QUETZ_INSN_VEC_MEM]);
}

QuetzCore::~QuetzCore() {
    delete pipeline_;
}

bool QuetzCore::isCoreHalted() const {
    return pipeline_->isHalted();
}

uint32_t QuetzCore::pendingCount() const {
    return pipeline_->pendingCount();
}

void QuetzCore::setMemLink(SST::Interfaces::StandardMem* link) {
    pipeline_->setMemLink(link);
}

void QuetzCore::finishCore() {
    pipeline_->finish();
}

void QuetzCore::handleMemResponse(SST::Interfaces::StandardMem::Request* resp) {
    uint64_t lat = 0;
    bool was_read = false;
    if (!pipeline_->handleResponse(resp, lat, was_read))
        return;
    if (was_read)
        stats_.read_lat->addData(lat);
    else
        stats_.write_lat->addData(lat);
}

void QuetzCore::tick() {
    pipeline_->tick();
}
