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
 * quetz_region_table.h — first-match lookup over MemRegionHandler instances.
 */

#ifndef _H_SST_QUETZ_REGION_TABLE
#define _H_SST_QUETZ_REGION_TABLE

#include <stdint.h>
#include <vector>

#include "quetz_region_handler.h"

namespace SST {
namespace Quetz {

class MemRegionTable {
public:
    MemRegionTable() = default;
    explicit MemRegionTable(std::vector<MemRegionHandler*> handlers);

    MemRegionHandler* findHandler(uint64_t addr) const;
    size_t            handlerCount() const { return handlers_.size(); }

    const std::vector<MemRegionHandler*>& handlers() const { return handlers_; }

private:
    std::vector<MemRegionHandler*> handlers_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_REGION_TABLE
