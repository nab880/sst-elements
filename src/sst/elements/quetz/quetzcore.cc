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

#include <algorithm>
#include <cstring>
#include <inttypes.h>
#include <vector>

using namespace SST;
using namespace SST::Quetz;
using namespace SST::Interfaces;

// ---------------------------------------------------------------------------
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
      mem_link_(nullptr),
      output_(out),
      tc_(tc),
      core_id_(coreID),
      max_pending_(maxPendTrans),
      pending_count_(0),
      max_issue_per_cycle_(maxIssuePerCycle),
      max_queue_len_(maxQueueLen),
      cache_line_size_(cacheLineSize),
      max_insts_(maxInsts),
      inst_count_(0),
      check_addresses_(checkAddresses),
      detailed_tracking_(detailedTracking),
      memmap_(memmap),
      halted_(false),
      stalled_(false)
{
    for (int c = 0; c < QUETZ_INSN_CLASS_COUNT; c++) {
        exec_latency_[c]     = execLatency[c];
        compute_latency_[c]  = computeLatency[c];
    }

    char sub_id[32];
    snprintf(sub_id, sizeof(sub_id), "%" PRIu32, coreID);

    stat_read_reqs_        = registerStatistic<uint64_t>("read_requests",       sub_id);
    stat_write_reqs_       = registerStatistic<uint64_t>("write_requests",      sub_id);
    stat_read_lat_         = registerStatistic<uint64_t>("read_latency",        sub_id);
    stat_write_lat_        = registerStatistic<uint64_t>("write_latency",       sub_id);
    stat_read_req_sizes_   = registerStatistic<uint64_t>("read_request_sizes",  sub_id);
    stat_write_req_sizes_  = registerStatistic<uint64_t>("write_request_sizes", sub_id);
    stat_split_reads_      = registerStatistic<uint64_t>("split_read_requests", sub_id);
    stat_split_writes_     = registerStatistic<uint64_t>("split_write_requests",sub_id);
    stat_noop_count_       = registerStatistic<uint64_t>("no_ops",              sub_id);
    stat_insn_count_       = registerStatistic<uint64_t>("instruction_count",   sub_id);
    stat_cycles_           = registerStatistic<uint64_t>("cycles",              sub_id);
    stat_active_cycles_    = registerStatistic<uint64_t>("active_cycles",       sub_id);
    stat_filtered_reads_   = registerStatistic<uint64_t>("filtered_reads",      sub_id);
    stat_filtered_writes_  = registerStatistic<uint64_t>("filtered_writes",     sub_id);
    stat_stall_cycles_         = registerStatistic<uint64_t>("stall_cycles",         sub_id);
    stat_compute_stall_cycles_ = registerStatistic<uint64_t>("compute_stall_cycles", sub_id);
    stat_int_compute_      = registerStatistic<uint64_t>("int_compute",         sub_id);
    stat_fp_compute_       = registerStatistic<uint64_t>("fp_compute",          sub_id);
    stat_vec_compute_      = registerStatistic<uint64_t>("vec_compute",         sub_id);
    stat_branch_           = registerStatistic<uint64_t>("branch",              sub_id);

    output_->verbose(CALL_INFO, 1, 0,
        "QuetzCore %" PRIu32 " created: maxPend=%" PRIu32
        " maxIssue=%" PRIu32 " maxQ=%" PRIu32 " clineSize=%" PRIu64
        " latency[int/fp/vec]=%" PRIu32 "/%" PRIu32 "/%" PRIu32
        " memmap_regions=%zu\n",
        core_id_, max_pending_, max_issue_per_cycle_,
        max_queue_len_, cache_line_size_,
        exec_latency_[QUETZ_INSN_INT_MEM],
        exec_latency_[QUETZ_INSN_FP_MEM],
        exec_latency_[QUETZ_INSN_VEC_MEM],
        memmap_.size());
}

QuetzCore::~QuetzCore() {}

// ---------------------------------------------------------------------------
void QuetzCore::finishCore() {
    output_->verbose(CALL_INFO, 1, 0,
        "QuetzCore %" PRIu32 " finishing, %" PRIu32
        " transactions still pending.\n",
        core_id_, pending_count_);
}

// ---------------------------------------------------------------------------
void QuetzCore::handleMemResponse(StandardMem::Request* resp) {
    auto it = pending_txns_.find(resp->getID());
    if (it == pending_txns_.end()) {
        // Untracked: post-halt drain or posted write ack.
        output_->verbose(CALL_INFO, 4, 0,
            "QuetzCore %" PRIu32 ": ignoring untracked response id %" PRIu64 "\n",
            core_id_, (uint64_t)resp->getID());
        delete resp;
        return;
    }

    uint64_t issue = it->second.issue_cycle;
    uint64_t now   = getCurrentSimTime(tc_);
    uint64_t lat   = (now >= issue) ? (now - issue) : 0;

    if (dynamic_cast<StandardMem::ReadResp*>(resp))
        stat_read_lat_->addData(lat);
    else
        stat_write_lat_->addData(lat);

    pending_txns_.erase(it);
    pending_count_--;
    delete resp;
}

// ---------------------------------------------------------------------------
// Drain commands from the shared-memory ring buffer into coreQ_
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Issue memory requests from coreQ_ up to the per-cycle limit.
// ---------------------------------------------------------------------------
void QuetzCore::processQueue() {
    uint32_t issued = 0;

    // Returns true and sets halted_ when the max-instruction limit is reached.
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

        // EXIT is never stalled — drain immediately.
        if (cmd.cmd == QUETZ_CMD_EXIT) {
            output_->verbose(CALL_INFO, 1, 0,
                "QuetzCore %" PRIu32 " processing EXIT — halting.\n", core_id_);
            halted_ = true;
            coreQ_.pop();
            return;
        }

        // Filtered memory accesses are dropped before the stall check: they
        // don't consume issue bandwidth, stall cycles, or pending slots.
        if ((cmd.cmd == QUETZ_CMD_READ || cmd.cmd == QUETZ_CMD_WRITE) &&
            isFiltered(cmd.addr))
        {
            if (cmd.cmd == QUETZ_CMD_WRITE)
                stat_filtered_writes_->addData(1);
            else
                stat_filtered_reads_->addData(1);
            stat_insn_count_->addData(1);
            coreQ_.pop();
            inst_count_++;
            if (checkMaxInsts()) return;
            continue;
        }

        // Stall check applies to both memory instructions (exec_latency) and
        // compute NOPs (compute_latency).
        if (sc.remaining_stall > 0) {
            sc.remaining_stall--;
            if (cmd.cmd == QUETZ_CMD_NOP)
                stat_compute_stall_cycles_->addData(1);
            else
                stat_stall_cycles_->addData(1);
            break;
        }

        if (cmd.cmd == QUETZ_CMD_NOP) {
            stat_noop_count_->addData(1);
            stat_insn_count_->addData(1);
            if (detailed_tracking_) {
                switch (static_cast<QuetzInsnClass>(cmd.insn_class)) {
                case QUETZ_INSN_INT_COMPUTE: stat_int_compute_->addData(1); break;
                case QUETZ_INSN_FP_COMPUTE:  stat_fp_compute_->addData(1);  break;
                case QUETZ_INSN_VEC_COMPUTE: stat_vec_compute_->addData(1); break;
                case QUETZ_INSN_BRANCH:      stat_branch_->addData(1);      break;
                default: break;
                }
            }
            coreQ_.pop();
            inst_count_++;
            if (checkMaxInsts()) return;
            continue;
        }

        uint32_t slots_needed = slotsNeeded(cmd.addr, cmd.size);

        if (issued + slots_needed > max_issue_per_cycle_)
            break;
        if (pending_count_ + slots_needed > max_pending_)
            break;

        switch (cmd.cmd) {
        case QUETZ_CMD_READ:
            issueRead(cmd.addr, cmd.size, cmd.pc);
            stat_insn_count_->addData(1);
            issued += slots_needed;
            coreQ_.pop();
            inst_count_++;
            if (checkMaxInsts()) return;
            break;

        case QUETZ_CMD_WRITE:
            issueWrite(cmd.addr, cmd.size, cmd.pc, cmd.data);
            stat_insn_count_->addData(1);
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
        stat_active_cycles_->addData(1);
    stat_cycles_->addData(1);
}

// ---------------------------------------------------------------------------
void QuetzCore::tick() {
    if (halted_)
        return;

    refillQueue();
    processQueue();
}

// ---------------------------------------------------------------------------
uint32_t QuetzCore::slotsNeeded(uint64_t vaddr, uint32_t size) const {
    if (size == 0) return 1;
    uint64_t line_end = (vaddr & ~(cache_line_size_ - 1)) + cache_line_size_;
    return (vaddr + size <= line_end) ? 1 : 2;
}

// ---------------------------------------------------------------------------
bool QuetzCore::isFiltered(uint64_t vaddr) const {
    for (const auto& r : memmap_) {
        if (vaddr >= r.start && vaddr <= r.end)
            return r.filtered;
    }
    return false;
}

// ---------------------------------------------------------------------------
void QuetzCore::issueRead(uint64_t vaddr, uint32_t size, uint64_t /*pc*/) {
    output_->verbose(CALL_INFO, 8, 0,
        "QuetzCore %" PRIu32 " READ  vaddr=0x%016" PRIx64 " size=%" PRIu32 "\n",
        core_id_, vaddr, size);

    if (check_addresses_ && size > (uint32_t)cache_line_size_)
        output_->verbose(CALL_INFO, 1, 0,
            "QuetzCore %" PRIu32 " READ vaddr=0x%016" PRIx64 " size=%" PRIu32
            " exceeds cache line size %" PRIu64 "; split may be incomplete\n",
            core_id_, vaddr, size, cache_line_size_);

    stat_read_req_sizes_->addData(size);

    uint64_t line_end = (vaddr & ~(cache_line_size_ - 1)) + cache_line_size_;

    auto send_read = [&](uint64_t addr, uint32_t len) {
        auto* req = new StandardMem::Read(addr, len, 0, addr);
        pending_txns_[req->getID()] = { req, getCurrentSimTime(tc_) };
        pending_count_++;
        stat_read_reqs_->addData(1);
        mem_link_->send(req);
    };

    if (vaddr + size <= line_end) {
        send_read(vaddr, size);
    } else {
        uint32_t first  = (uint32_t)(line_end - vaddr);
        uint32_t second = size - first;
        send_read(vaddr,         first);
        send_read(vaddr + first, second);
        stat_split_reads_->addData(1);
    }
}

// ---------------------------------------------------------------------------
void QuetzCore::issueWrite(uint64_t vaddr, uint32_t size, uint64_t /*pc*/,
                           const uint8_t* raw_data) {
    output_->verbose(CALL_INFO, 8, 0,
        "QuetzCore %" PRIu32 " WRITE vaddr=0x%016" PRIx64 " size=%" PRIu32 "\n",
        core_id_, vaddr, size);

    if (check_addresses_ && size > (uint32_t)cache_line_size_)
        output_->verbose(CALL_INFO, 1, 0,
            "QuetzCore %" PRIu32 " WRITE vaddr=0x%016" PRIx64 " size=%" PRIu32
            " exceeds cache line size %" PRIu64 "; split may be incomplete\n",
            core_id_, vaddr, size, cache_line_size_);

    stat_write_req_sizes_->addData(size);

    uint64_t line_end = (vaddr & ~(cache_line_size_ - 1)) + cache_line_size_;

    static constexpr uint32_t kDataCap = (uint32_t)sizeof(QuetzCommand::data);
    auto send_write = [&](uint64_t addr, uint32_t len, uint32_t offset) {
        std::vector<uint8_t> data(len, 0);
        if (raw_data) {
            uint32_t avail  = (offset < kDataCap) ? (kDataCap - offset) : 0;
            uint32_t copy_n = (len < avail) ? len : avail;
            if (copy_n > 0)
                memcpy(data.data(), raw_data + offset, copy_n);
        }
        auto* req = new StandardMem::Write(addr, len, data, false, 0, addr);
        pending_txns_[req->getID()] = { req, getCurrentSimTime(tc_) };
        pending_count_++;
        stat_write_reqs_->addData(1);
        mem_link_->send(req);
    };

    if (vaddr + size <= line_end) {
        send_write(vaddr, size, 0);
    } else {
        uint32_t first  = (uint32_t)(line_end - vaddr);
        uint32_t second = size - first;
        send_write(vaddr,         first,  0);
        send_write(vaddr + first, second, first);
        stat_split_writes_->addData(1);
    }
}
