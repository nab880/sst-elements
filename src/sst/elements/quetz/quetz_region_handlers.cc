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

#include "quetz_region_handlers.h"

#include <inttypes.h>

using namespace SST;
using namespace SST::Quetz;

BoundedRegionHandler::BoundedRegionHandler(ComponentId_t id, Params& params)
    : MemRegionHandler(id, params),
      start_(params.find<uint64_t>("start", 0)),
      end_(params.find<uint64_t>("end", 0))
{}

// ---------------------------------------------------------------------------
ForwardRegionHandler::ForwardRegionHandler(ComponentId_t id, Params& params)
    : BoundedRegionHandler(id, params)
{}

MemRegionHandler::Action
ForwardRegionHandler::onRead(const QuetzCommand& /*cmd*/, QuetzCoreStats& /*stats*/)
{
    return Action::FORWARD;
}

MemRegionHandler::Action
ForwardRegionHandler::onWrite(const QuetzCommand& /*cmd*/, QuetzCoreStats& /*stats*/)
{
    return Action::FORWARD;
}

// ---------------------------------------------------------------------------
FilteredRegionHandler::FilteredRegionHandler(ComponentId_t id, Params& params)
    : BoundedRegionHandler(id, params)
{}

MemRegionHandler::Action
FilteredRegionHandler::onRead(const QuetzCommand& /*cmd*/, QuetzCoreStats& stats)
{
    stats.filtered_reads->addData(1);
    return Action::CONSUME;
}

MemRegionHandler::Action
FilteredRegionHandler::onWrite(const QuetzCommand& /*cmd*/, QuetzCoreStats& stats)
{
    stats.filtered_writes->addData(1);
    return Action::CONSUME;
}

// ---------------------------------------------------------------------------
UartRegionHandler::UartRegionHandler(ComponentId_t id, Params& params)
    : BoundedRegionHandler(id, params),
      tx_offset_(params.find<uint32_t>("tx_offset", 0))
{}

MemRegionHandler::Action
UartRegionHandler::onRead(const QuetzCommand& /*cmd*/, QuetzCoreStats& stats)
{
    stats.filtered_reads->addData(1);
    return Action::CONSUME;
}

MemRegionHandler::Action
UartRegionHandler::onWrite(const QuetzCommand& cmd, QuetzCoreStats& stats)
{
    if (cmd.size >= 1) {
        uint64_t offset = cmd.addr - start_;
        if (offset == tx_offset_)
            uart_tx_buf_ += static_cast<char>(cmd.data[0]);
    }
    stats.filtered_writes->addData(1);
    return Action::CONSUME;
}

void UartRegionHandler::finish(SST::Output* out, uint32_t core_id) {
    if (!uart_tx_buf_.empty())
        out->output("UART[%" PRIu32 "]: %s\n", core_id, uart_tx_buf_.c_str());
}

// ---------------------------------------------------------------------------
MmioForwardRegionHandler::MmioForwardRegionHandler(ComponentId_t id, Params& params)
    : BoundedRegionHandler(id, params)
{}

MemRegionHandler::Action
MmioForwardRegionHandler::onRead(const QuetzCommand& /*cmd*/, QuetzCoreStats& /*stats*/)
{
    return Action::FORWARD;
}

MemRegionHandler::Action
MmioForwardRegionHandler::onWrite(const QuetzCommand& /*cmd*/, QuetzCoreStats& /*stats*/)
{
    return Action::FORWARD;
}
