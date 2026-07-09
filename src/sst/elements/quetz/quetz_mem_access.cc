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
        const MemRegionTable& table, bool filter_unmatched)
    : table_(table),
      handlers_for_finish_(table.handlers()),
      filter_unmatched_(filter_unmatched)
{}

MemRegionHandler::Action
RegionTableMemAccessStrategy::handleMemoryAccess(const QuetzCommand& cmd,
                                                 QuetzCoreStats& stats) {
    if (cmd.cmd != QUETZ_CMD_READ && cmd.cmd != QUETZ_CMD_WRITE)
        return MemRegionHandler::Action::FORWARD;

    MemRegionHandler* h = table_.findHandler(cmd.addr);
    if (!h) {
        // filter_unmatched: the deck declared its memory map, so an address
        // no handler owns is wild guest traffic — count it and consume it
        // instead of forwarding into memHierarchy, where an unowned address
        // is a routing fatal. Same observable behavior as the full-range
        // FilteredRegionHandler decks used to append by hand.
        if (filter_unmatched_) {
            ((cmd.cmd == QUETZ_CMD_READ) ? stats.filtered_reads
                                         : stats.filtered_writes)->addData(1);
            return MemRegionHandler::Action::CONSUME;
        }
        return MemRegionHandler::Action::FORWARD;
    }

    return (cmd.cmd == QUETZ_CMD_READ) ? h->onRead(cmd, stats) : h->onWrite(cmd, stats);
}

void RegionTableMemAccessStrategy::finish(SST::Output* out, uint32_t core_id) {
    for (MemRegionHandler* h : handlers_for_finish_)
        h->finish(out, core_id);
}
