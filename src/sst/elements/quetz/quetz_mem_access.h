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



#ifndef _H_SST_QUETZ_MEM_ACCESS
#define _H_SST_QUETZ_MEM_ACCESS

#include <sst/core/output.h>

#include <vector>

#include "quetz_memmap.h"
#include "quetz_shmem.h"
#include "quetz_stats.h"

namespace SST {
namespace Quetz {

class MemAccessStrategy {
public:
    virtual ~MemAccessStrategy() = default;

    
    virtual bool handleMemoryAccess(const QuetzCommand& cmd,
                                    QuetzCoreStats& stats) = 0;

    virtual void finish(SST::Output* out, uint32_t core_id) = 0;
};

class MemMapMemAccessStrategy : public MemAccessStrategy {
public:
    explicit MemMapMemAccessStrategy(std::vector<MemRegion> regions);

    bool handleMemoryAccess(const QuetzCommand& cmd,
                            QuetzCoreStats& stats) override;
    void finish(SST::Output* out, uint32_t core_id) override;

    size_t regionCount() const { return memmap_.regionCount(); }

private:
    MemMap memmap_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_MEM_ACCESS
