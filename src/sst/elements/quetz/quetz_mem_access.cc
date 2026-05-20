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

#include "quetz_mem_access.h"

using namespace SST;
using namespace SST::Quetz;

MemMapMemAccessStrategy::MemMapMemAccessStrategy(std::vector<MemRegion> regions)
    : memmap_(std::move(regions))
{}

bool MemMapMemAccessStrategy::handleMemoryAccess(const QuetzCommand& cmd,
                                                 QuetzCoreStats& stats) {
    if (cmd.cmd != QUETZ_CMD_READ && cmd.cmd != QUETZ_CMD_WRITE)
        return false;
    if (!memmap_.isFiltered(cmd.addr))
        return false;

    if (cmd.cmd == QUETZ_CMD_WRITE) {
        if (cmd.size >= 1)
            memmap_.captureUartByte(cmd.addr, cmd.data[0]);
        stats.filtered_writes->addData(1);
    } else {
        stats.filtered_reads->addData(1);
    }
    return true;
}

void MemMapMemAccessStrategy::finish(SST::Output* out, uint32_t core_id) {
    memmap_.flushUart(out, core_id);
}
