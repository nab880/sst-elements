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

#include "quetz_region_table.h"

using namespace SST::Quetz;

MemRegionTable::MemRegionTable(std::vector<MemRegionHandler*> handlers)
    : handlers_(std::move(handlers))
{}

MemRegionHandler* MemRegionTable::findHandler(uint64_t addr) const {
    for (MemRegionHandler* h : handlers_) {
        if (h && addr >= h->startAddr() && addr <= h->endAddr())
            return h;
    }
    return nullptr;
}
