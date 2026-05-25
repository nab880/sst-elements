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

#include <algorithm>
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
GpuTraceRegionHandler::GpuTraceRegionHandler(ComponentId_t id, Params& params)
    : BoundedRegionHandler(id, params),
      doorbell_offset_(params.find<uint32_t>("doorbell_offset", 0)),
      status_offset_(params.find<uint32_t>("status_offset", 8)),
      max_payload_log_(params.find<uint32_t>("max_payload_log", 8)),
      doorbell_count_(0),
      poll_count_(0)
{}

uint64_t GpuTraceRegionHandler::decodeDoorbellLo(const QuetzCommand& cmd)
{
    uint64_t val = 0;
    uint32_t cap = static_cast<uint32_t>(
        std::min<size_t>(sizeof(cmd.data), sizeof(uint64_t)));
    uint32_t n = cmd.size < cap ? cmd.size : cap;
    for (uint32_t i = 0; i < n; ++i)
        val |= static_cast<uint64_t>(cmd.data[i]) << (8 * i);
    return val;
}

void GpuTraceRegionHandler::recordDoorbellPayload(uint64_t payload)
{
    recent_doorbell_lo_.push_back(payload);
    if (recent_doorbell_lo_.size() > max_payload_log_)
        recent_doorbell_lo_.pop_front();
}

MemRegionHandler::Action
GpuTraceRegionHandler::onRead(const QuetzCommand& cmd, QuetzCoreStats& stats)
{
    uint64_t offset = cmd.addr - start_;
    if (offset == status_offset_) {
        stats.gpu_status_polls->addData(1);
        ++poll_count_;
    } else {
        stats.gpu_other_reads->addData(1);
    }
    return Action::CONSUME;
}

MemRegionHandler::Action
GpuTraceRegionHandler::onWrite(const QuetzCommand& cmd, QuetzCoreStats& stats)
{
    uint64_t offset = cmd.addr - start_;
    if (offset == doorbell_offset_) {
        stats.gpu_doorbell_writes->addData(1);
        ++doorbell_count_;
        recordDoorbellPayload(decodeDoorbellLo(cmd));
    } else {
        stats.gpu_other_writes->addData(1);
    }
    return Action::CONSUME;
}

void GpuTraceRegionHandler::finish(SST::Output* out, uint32_t core_id)
{
    if (doorbell_count_ == 0 && poll_count_ == 0)
        return;

    out->output("GPU_TRACE[%" PRIu32 "]: doorbells=%" PRIu64
                " polls=%" PRIu64 " last_doorbells={",
                core_id, doorbell_count_, poll_count_);

    for (size_t i = 0; i < recent_doorbell_lo_.size(); ++i) {
        if (i > 0)
            out->output(",");
        out->output("0x%" PRIx64, recent_doorbell_lo_[i]);
    }
    out->output("}\n");
}

// ---------------------------------------------------------------------------
MmioForwardRegionHandler::MmioForwardRegionHandler(ComponentId_t id, Params& params)
    : BoundedRegionHandler(id, params)
{}

MemRegionHandler::Action
MmioForwardRegionHandler::onRead(const QuetzCommand& /*cmd*/, QuetzCoreStats& /*stats*/)
{
    return Action::FORWARD_MMIO;
}

MemRegionHandler::Action
MmioForwardRegionHandler::onWrite(const QuetzCommand& /*cmd*/, QuetzCoreStats& /*stats*/)
{
    return Action::FORWARD_MMIO;
}
