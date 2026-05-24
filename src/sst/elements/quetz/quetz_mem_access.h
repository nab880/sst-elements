// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

/**
 * quetz_mem_access.h — strategy for handling guest memory accesses on the
 * SST side before they reach memHierarchy.
 */

#ifndef _H_SST_QUETZ_MEM_ACCESS
#define _H_SST_QUETZ_MEM_ACCESS

#include <sst/core/output.h>

#include <vector>

#include "quetz_region_handler.h"
#include "quetz_region_table.h"
#include "quetz_shmem.h"
#include "quetz_stats.h"

namespace SST {
namespace Quetz {

class MemAccessStrategy {
public:
    virtual ~MemAccessStrategy() = default;

    /**
     * Inspect a READ or WRITE command.
     * @return CONSUME if handled locally; FORWARD_MMIO for MMIO range;
     *         FORWARD for cached hierarchy path (also when no handler matches).
     */
    virtual MemRegionHandler::Action handleMemoryAccess(const QuetzCommand& cmd,
                                                        QuetzCoreStats& stats) = 0;

    virtual void finish(SST::Output* out, uint32_t core_id) = 0;
};

class RegionTableMemAccessStrategy : public MemAccessStrategy {
public:
    explicit RegionTableMemAccessStrategy(const MemRegionTable& table);

    MemRegionHandler::Action handleMemoryAccess(const QuetzCommand& cmd,
                                                QuetzCoreStats& stats) override;
    void finish(SST::Output* out, uint32_t core_id) override;

    size_t handlerCount() const { return table_.handlerCount(); }

private:
    const MemRegionTable& table_;
    std::vector<MemRegionHandler*> handlers_for_finish_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_MEM_ACCESS
