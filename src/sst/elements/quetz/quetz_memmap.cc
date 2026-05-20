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

#include "quetz_memmap.h"

#include <inttypes.h>

using namespace SST;
using namespace SST::Quetz;

MemMap::MemMap(std::vector<MemRegion> regions)
    : regions_(std::move(regions))
{}

const MemRegion* MemMap::findRegion(uint64_t vaddr) const {
    for (const auto& r : regions_)
        if (vaddr >= r.start && vaddr <= r.end)
            return &r;
    return nullptr;
}

bool MemMap::isFiltered(uint64_t vaddr) const {
    const MemRegion* r = findRegion(vaddr);
    return r && r->type != MemRegionType::MEMORY;
}

bool MemMap::captureUartByte(uint64_t addr, uint8_t byte) {
    const MemRegion* r = findRegion(addr);
    if (!r || r->type != MemRegionType::UART)
        return false;
    uint64_t offset = addr - r->start;
    if (offset != r->uart_tx_offset)
        return false;
    uart_tx_buf_ += static_cast<char>(byte);
    return true;
}

void MemMap::flushUart(SST::Output* out, uint32_t core_id) const {
    if (!uart_tx_buf_.empty())
        out->output("UART[%" PRIu32 "]: %s\n", core_id, uart_tx_buf_.c_str());
}
