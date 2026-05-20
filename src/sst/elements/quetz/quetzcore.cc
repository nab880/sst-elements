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

using namespace SST;
using namespace SST::Quetz;

QuetzCore::QuetzCore(
        ComponentId_t                id,
        QuetzTunnel*                 tunnel,
        uint32_t                     coreID,
        uint32_t                     maxPendTrans,
        SST::Output*                 out,
        uint32_t                     maxIssuePerCycle,
        uint32_t                     maxQueueLen,
        uint64_t                     cacheLineSize,
        TimeConverter                tc,
        Params&                      /*params*/,
        const uint32_t               execLatency[QUETZ_INSN_CLASS_COUNT],
        const uint32_t               computeLatency[QUETZ_INSN_CLASS_COUNT],
        const std::vector<MemRegion>& memmap,
        uint64_t                     maxInsts,
        uint32_t                     checkAddresses,
        bool                         detailedTracking)
    : ComponentExtension(id),
      tunnel_(tunnel),
      output_(out),
      tc_(tc),
      core_id_(coreID),
      max_pending_(maxPendTrans),
      max_issue_per_cycle_(maxIssuePerCycle),
      max_queue_len_(maxQueueLen),
      max_insts_(maxInsts),
      inst_count_(0),
      detailed_tracking_(detailedTracking),
      halted_(false),
      stalled_(false),
      memmap_(memmap),
      emitter_(this, out, coreID, tc, cacheLineSize, checkAddresses, stats_)
{
    for (int c = 0; c < QUETZ_INSN_CLASS_COUNT; c++) {
        exec_latency_[c]    = execLatency[c];
        compute_latency_[c] = computeLatency[c];
    }

    char sub_id[32];
    snprintf(sub_id, sizeof(sub_id), "%" PRIu32, coreID);
    stats_.registerAll(this, sub_id);

    output_->verbose(CALL_INFO, 1, 0,
        "QuetzCore %" PRIu32 " created: maxPend=%" PRIu32
        " maxIssue=%" PRIu32 " maxQ=%" PRIu32 " clineSize=%" PRIu64
        " latency[int/fp/vec]=%" PRIu32 "/%" PRIu32 "/%" PRIu32
        " memmap_regions=%zu\n",
        core_id_, max_pending_, max_issue_per_cycle_,
        max_queue_len_, cacheLineSize,
        exec_latency_[QUETZ_INSN_INT_MEM],
        exec_latency_[QUETZ_INSN_FP_MEM],
        exec_latency_[QUETZ_INSN_VEC_MEM],
        memmap_.regionCount());
}

QuetzCore::~QuetzCore() {}

void QuetzCore::setMemLink(SST::Interfaces::StandardMem* link) {
    emitter_.setLink(link);
}

void QuetzCore::finishCore() {
    output_->verbose(CALL_INFO, 1, 0,
        "QuetzCore %" PRIu32 " finishing, %" PRIu32
        " transactions still pending.\n",
        core_id_, emitter_.pendingCount());
    memmap_.flushUart(output_, core_id_);
}

void QuetzCore::handleMemResponse(SST::Interfaces::StandardMem::Request* resp) {
    uint64_t lat = 0;
    bool was_read = false;
    if (!emitter_.handleResponse(resp, lat, was_read))
        return;
    if (was_read)
        stats_.read_lat->addData(lat);
    else
        stats_.write_lat->addData(lat);
}

void QuetzCore::refillQueue() {
    while (coreQ_.size() < max_queue_len_) {
        QuetzCommand cmd;
        if (!tunnel_->readMessageNB((size_t)core_id_, &cmd))
            break;

        uint32_t stall = 0;
        if (cmd.insn_class < (uint32_t)QUETZ_INSN_CLASS_COUNT) {
            if (cmd.cmd == QUETZ_CMD_READ || cmd.cmd == QUETZ_CMD_WRITE)
                stall = exec_latency_[cmd.insn_class];
            else if (cmd.cmd == QUETZ_CMD_NOP)
                stall = compute_latency_[cmd.insn_class];
        }

        coreQ_.push({ cmd, stall });

        if (cmd.cmd == QUETZ_CMD_EXIT) {
            output_->verbose(CALL_INFO, 1, 0,
                "QuetzCore %" PRIu32 " received EXIT from plugin.\n", core_id_);
            break;
        }
    }
}

void QuetzCore::processQueue() {
    uint32_t issued = 0;

    auto checkMaxInsts = [&]() -> bool {
        if (max_insts_ > 0 && inst_count_ >= max_insts_) {
            output_->verbose(CALL_INFO, 1, 0,
                "QuetzCore %" PRIu32 " reached max_insts %" PRIu64 " — halting.\n",
                core_id_, max_insts_);
            halted_ = true;
            return true;
        }
        return false;
    };

    while (!coreQ_.empty()) {
        StagedCmd& sc     = coreQ_.front();
        QuetzCommand& cmd = sc.cmd;

        if (cmd.cmd == QUETZ_CMD_EXIT) {
            output_->verbose(CALL_INFO, 1, 0,
                "QuetzCore %" PRIu32 " processing EXIT — halting.\n", core_id_);
            halted_ = true;
            coreQ_.pop();
            return;
        }

        if ((cmd.cmd == QUETZ_CMD_READ || cmd.cmd == QUETZ_CMD_WRITE) &&
            memmap_.isFiltered(cmd.addr))
        {
            if (cmd.cmd == QUETZ_CMD_WRITE) {
                if (cmd.size >= 1)
                    memmap_.captureUartByte(cmd.addr, cmd.data[0]);
                stats_.filtered_writes->addData(1);
            } else {
                stats_.filtered_reads->addData(1);
            }
            stats_.insn_count->addData(1);
            coreQ_.pop();
            inst_count_++;
            if (checkMaxInsts()) return;
            continue;
        }

        if (sc.remaining_stall > 0) {
            sc.remaining_stall--;
            if (cmd.cmd == QUETZ_CMD_NOP)
                stats_.compute_stall_cycles->addData(1);
            else
                stats_.stall_cycles->addData(1);
            break;
        }

        if (cmd.cmd == QUETZ_CMD_NOP) {
            stats_.noop_count->addData(1);
            stats_.insn_count->addData(1);
            if (detailed_tracking_) {
                switch (static_cast<QuetzInsnClass>(cmd.insn_class)) {
                case QUETZ_INSN_INT_COMPUTE: stats_.int_compute->addData(1); break;
                case QUETZ_INSN_FP_COMPUTE:  stats_.fp_compute->addData(1);  break;
                case QUETZ_INSN_VEC_COMPUTE: stats_.vec_compute->addData(1); break;
                case QUETZ_INSN_BRANCH:      stats_.branch->addData(1);      break;
                default: break;
                }
            }
            coreQ_.pop();
            inst_count_++;
            if (checkMaxInsts()) return;
            continue;
        }

        uint32_t slots_needed = emitter_.slotsNeeded(cmd.addr, cmd.size);

        if (issued + slots_needed > max_issue_per_cycle_)
            break;
        if (emitter_.pendingCount() + slots_needed > max_pending_)
            break;

        switch (cmd.cmd) {
        case QUETZ_CMD_READ:
            emitter_.issueRead(cmd.addr, cmd.size, cmd.pc);
            stats_.insn_count->addData(1);
            issued += slots_needed;
            coreQ_.pop();
            inst_count_++;
            if (checkMaxInsts()) return;
            break;

        case QUETZ_CMD_WRITE:
            emitter_.issueWrite(cmd.addr, cmd.size, cmd.pc, cmd.data);
            stats_.insn_count->addData(1);
            issued += slots_needed;
            coreQ_.pop();
            inst_count_++;
            if (checkMaxInsts()) return;
            break;

        default:
            output_->fatal(CALL_INFO, -1,
                "QuetzCore %" PRIu32 ": unknown command %" PRIu32 "\n",
                core_id_, (uint32_t)cmd.cmd);
        }
    }

    if (issued > 0)
        stats_.active_cycles->addData(1);
    stats_.cycles->addData(1);
}

void QuetzCore::tick() {
    if (halted_)
        return;
    refillQueue();
    processQueue();
}
