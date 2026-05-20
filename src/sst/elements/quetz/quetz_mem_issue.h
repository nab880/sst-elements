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

#ifndef _H_SST_QUETZ_MEM_ISSUE
#define _H_SST_QUETZ_MEM_ISSUE

#include <sst/core/componentExtension.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>

#include <stdint.h>
#include <unordered_map>

#include "quetz_shmem.h"
#include "quetz_stats.h"

namespace SST {
namespace Quetz {

struct QuetzPendingReq {
    SST::Interfaces::StandardMem::Request* req;
    uint64_t issue_cycle;
};

class MemRequestEmitter {
public:
    MemRequestEmitter(
        ComponentExtension*              comp,
        SST::Output*                     out,
        uint32_t                         core_id,
        SST::TimeConverter               tc,
        uint64_t                         cache_line_size,
        uint32_t                         check_addresses,
        QuetzCoreStats&                  stats);

    void setLink(SST::Interfaces::StandardMem* link) { mem_link_ = link; }

    uint32_t slotsNeeded(uint64_t vaddr, uint32_t size) const;
    void issueRead (uint64_t vaddr, uint32_t size, uint64_t pc);
    void issueWrite(uint64_t vaddr, uint32_t size, uint64_t pc,
                    const uint8_t* raw_data = nullptr);

    bool handleResponse(SST::Interfaces::StandardMem::Request* resp,
                        uint64_t& latency_out, bool& was_read_out);

    uint32_t pendingCount() const { return pending_count_; }

private:
    ComponentExtension*               comp_;
    SST::Interfaces::StandardMem*     mem_link_;
    SST::Output*                      output_;
    uint32_t                          core_id_;
    SST::TimeConverter                tc_;
    uint64_t                          cache_line_size_;
    uint32_t                          check_addresses_;
    QuetzCoreStats&                   stats_;
    uint32_t                          pending_count_;

    std::unordered_map<SST::Interfaces::StandardMem::Request::id_t,
                       QuetzPendingReq> pending_txns_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_MEM_ISSUE
