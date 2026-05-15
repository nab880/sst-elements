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
 * quetzcore.h — per-vCPU event pump for the Quetz SST component.
 *
 * On every SST clock tick the owning QuetzCPU calls tick() on each
 * QuetzCore.  QuetzCore drains commands from its shared-memory buffer,
 * stages them in a local queue, and issues up to maxIssuePer Cycle memory
 * requests through its StandardMem interface.
 *
 * Design mirrors ariel/arielcore.h so that existing SST infrastructure
 * (statistics, memHierarchy wiring, etc.) can be reused without modification.
 */

#ifndef _H_SST_QUETZ_CORE
#define _H_SST_QUETZ_CORE

#include <sst/core/componentExtension.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>
#include <sst/core/params.h>
#include <sst/core/timeConverter.h>

#include <stdint.h>
#include <queue>
#include <unordered_map>
#include <vector>

#include "quetz_shmem.h"

namespace SST {
namespace Quetz {

// ---------------------------------------------------------------------------
// Named address-range region
// ---------------------------------------------------------------------------
enum class MemRegionType {
    MEMORY,    // forward to memory hierarchy (default)
    FILTERED,  // count only, do not forward
    UART,      // capture TX bytes; do not forward to hierarchy
};

struct MemRegion {
    std::string    name;
    uint64_t       start;
    uint64_t       end;           // inclusive
    MemRegionType  type;
    uint32_t       uart_tx_offset; // byte offset of TX data register within region
};

// ---------------------------------------------------------------------------
// Pending-transaction bookkeeping (mirrors Ariel's RequestInfo)
// ---------------------------------------------------------------------------
struct QuetzPendingReq {
    SST::Interfaces::StandardMem::Request* req;
    uint64_t issue_cycle;
};

// ---------------------------------------------------------------------------
// Queue entry: wraps a plugin command with an execution-stall countdown
// ---------------------------------------------------------------------------
struct StagedCmd {
    QuetzCommand cmd;
    uint32_t    remaining_stall;
};

// ---------------------------------------------------------------------------
// QuetzCore — one instance per guest vCPU
// ---------------------------------------------------------------------------
class QuetzCore : public SST::ComponentExtension {
public:
    QuetzCore(
        ComponentId_t                   id,
        QuetzTunnel*                    tunnel,
        uint32_t                        coreID,
        uint32_t                        maxPendTrans,
        SST::Output*                    out,
        uint32_t                        maxIssuePerCycle,
        uint32_t                        maxQueueLen,
        uint64_t                        cacheLineSize,
        TimeConverter                   tc,
        Params&                         params,
        const uint32_t                  execLatency[QUETZ_INSN_CLASS_COUNT],
        const uint32_t                  computeLatency[QUETZ_INSN_CLASS_COUNT],
        const std::vector<MemRegion>&   memmap,
        uint64_t                        maxInsts,
        uint32_t                        checkAddresses,
        bool                            detailedTracking);

    ~QuetzCore();

    void setMemLink(SST::Interfaces::StandardMem* link) {
        mem_link_ = link;
    }

    // Called each SST clock cycle by QuetzCPU::tick().
    void tick();

    // Response handler — wired up as the StandardMem callback.
    void handleMemResponse(SST::Interfaces::StandardMem::Request* resp);

    bool isCoreHalted()  const { return halted_;  }
    bool isCoreStalled() const { return stalled_; }

    void finishCore();

private:
    void refillQueue();
    void processQueue();

    void issueRead (uint64_t vaddr, uint32_t size, uint64_t pc);
    void issueWrite(uint64_t vaddr, uint32_t size, uint64_t pc,
                    const uint8_t* raw_data = nullptr);
    uint32_t          slotsNeeded(uint64_t vaddr, uint32_t size) const;
    const MemRegion*  findRegion (uint64_t vaddr) const;
    bool              isFiltered (uint64_t vaddr) const;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    QuetzTunnel*                         tunnel_;
    SST::Interfaces::StandardMem*        mem_link_;
    SST::Output*                         output_;
    TimeConverter                        tc_;

    uint32_t core_id_;
    uint32_t max_pending_;
    uint32_t pending_count_;
    uint32_t max_issue_per_cycle_;
    uint32_t max_queue_len_;
    uint64_t cache_line_size_;

    uint64_t max_insts_;
    uint64_t inst_count_;
    uint32_t check_addresses_;
    bool     detailed_tracking_;

    bool        halted_;
    bool        stalled_;

    std::string uart_tx_buf_;     // accumulated guest UART TX bytes

    std::queue<StagedCmd> coreQ_;

    std::unordered_map<SST::Interfaces::StandardMem::Request::id_t,
                       QuetzPendingReq> pending_txns_;

    uint32_t               exec_latency_[QUETZ_INSN_CLASS_COUNT];
    uint32_t               compute_latency_[QUETZ_INSN_CLASS_COUNT];
    std::vector<MemRegion> memmap_;

    // -----------------------------------------------------------------------
    // Statistics
    // -----------------------------------------------------------------------
    SST::Statistics::Statistic<uint64_t>* stat_read_reqs_;
    SST::Statistics::Statistic<uint64_t>* stat_write_reqs_;
    SST::Statistics::Statistic<uint64_t>* stat_read_lat_;
    SST::Statistics::Statistic<uint64_t>* stat_write_lat_;
    SST::Statistics::Statistic<uint64_t>* stat_read_req_sizes_;
    SST::Statistics::Statistic<uint64_t>* stat_write_req_sizes_;
    SST::Statistics::Statistic<uint64_t>* stat_split_reads_;
    SST::Statistics::Statistic<uint64_t>* stat_split_writes_;
    SST::Statistics::Statistic<uint64_t>* stat_noop_count_;
    SST::Statistics::Statistic<uint64_t>* stat_insn_count_;
    SST::Statistics::Statistic<uint64_t>* stat_cycles_;
    SST::Statistics::Statistic<uint64_t>* stat_active_cycles_;
    SST::Statistics::Statistic<uint64_t>* stat_filtered_reads_;
    SST::Statistics::Statistic<uint64_t>* stat_filtered_writes_;
    SST::Statistics::Statistic<uint64_t>* stat_stall_cycles_;
    SST::Statistics::Statistic<uint64_t>* stat_compute_stall_cycles_;
    SST::Statistics::Statistic<uint64_t>* stat_int_compute_;
    SST::Statistics::Statistic<uint64_t>* stat_fp_compute_;
    SST::Statistics::Statistic<uint64_t>* stat_vec_compute_;
    SST::Statistics::Statistic<uint64_t>* stat_branch_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_CORE
