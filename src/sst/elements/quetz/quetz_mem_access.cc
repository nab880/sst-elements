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

RegionTableMemAccessStrategy::RegionTableMemAccessStrategy(
        const MemRegionTable& table)
    : table_(table),
      handlers_for_finish_(table.handlers())
{}

bool RegionTableMemAccessStrategy::handleMemoryAccess(const QuetzCommand& cmd,
                                                      QuetzCoreStats& stats) {
    if (cmd.cmd != QUETZ_CMD_READ && cmd.cmd != QUETZ_CMD_WRITE)
        return false;

    MemRegionHandler* h = table_.findHandler(cmd.addr);
    if (!h)
        return false;

    MemRegionHandler::Action act =
        (cmd.cmd == QUETZ_CMD_READ) ? h->onRead(cmd, stats) : h->onWrite(cmd, stats);
    return (act == MemRegionHandler::Action::CONSUME);
}

void RegionTableMemAccessStrategy::finish(SST::Output* out, uint32_t core_id) {
    for (MemRegionHandler* h : handlers_for_finish_)
        h->finish(out, core_id);
}
