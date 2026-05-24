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

#ifndef _H_SST_QUETZ_REGION_HANDLERS
#define _H_SST_QUETZ_REGION_HANDLERS

#include "quetz_region_handler.h"

#include <string>
#include <vector>

namespace SST {
namespace Quetz {

/** Base for handlers configured with start/end Params. */
class BoundedRegionHandler : public MemRegionHandler {
public:
    BoundedRegionHandler(ComponentId_t id, Params& params);

    uint64_t startAddr() const override { return start_; }
    uint64_t endAddr()   const override { return end_; }

protected:
    uint64_t start_;
    uint64_t end_;
};

class ForwardRegionHandler : public BoundedRegionHandler {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        ForwardRegionHandler,
        "quetz",
        "ForwardRegionHandler",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Forward guest memory traffic to the cache hierarchy.",
        SST::Quetz::MemRegionHandler)

    SST_ELI_DOCUMENT_PARAMS(
        { "start", "Inclusive region start address.", "0" },
        { "end",   "Inclusive region end address.",   "0" })

    ForwardRegionHandler(ComponentId_t id, Params& params);

    Action onRead(const QuetzCommand& cmd, QuetzCoreStats& stats) override;
    Action onWrite(const QuetzCommand& cmd, QuetzCoreStats& stats) override;
};

class FilteredRegionHandler : public BoundedRegionHandler {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        FilteredRegionHandler,
        "quetz",
        "FilteredRegionHandler",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Count filtered_reads/filtered_writes; do not forward to hierarchy.",
        SST::Quetz::MemRegionHandler)

    SST_ELI_DOCUMENT_PARAMS(
        { "start", "Inclusive region start address.", "0" },
        { "end",   "Inclusive region end address.",   "0" })

    FilteredRegionHandler(ComponentId_t id, Params& params);

    Action onRead(const QuetzCommand& cmd, QuetzCoreStats& stats) override;
    Action onWrite(const QuetzCommand& cmd, QuetzCoreStats& stats) override;
};

class UartRegionHandler : public BoundedRegionHandler {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        UartRegionHandler,
        "quetz",
        "UartRegionHandler",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Capture UART TX writes; do not forward to hierarchy.",
        SST::Quetz::MemRegionHandler)

    SST_ELI_DOCUMENT_PARAMS(
        { "start",      "Inclusive region start address.", "0" },
        { "end",        "Inclusive region end address.",   "0" },
        { "tx_offset",  "Byte offset of TX data register within region.", "0" })

    UartRegionHandler(ComponentId_t id, Params& params);

    Action onRead(const QuetzCommand& cmd, QuetzCoreStats& stats) override;
    Action onWrite(const QuetzCommand& cmd, QuetzCoreStats& stats) override;
    void   finish(SST::Output* out, uint32_t core_id) override;

private:
    uint32_t    tx_offset_;
    std::string uart_tx_buf_;
};

class GpuTraceRegionHandler : public BoundedRegionHandler {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        GpuTraceRegionHandler,
        "quetz",
        "GpuTraceRegionHandler",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Trace GPU MMIO doorbell/status accesses; do not forward.",
        SST::Quetz::MemRegionHandler)

    SST_ELI_DOCUMENT_PARAMS(
        { "start",            "Inclusive region start address.",            "0" },
        { "end",              "Inclusive region end address.",              "0" },
        { "doorbell_offset",  "Byte offset of doorbell register within region.", "0" },
        { "status_offset",    "Byte offset of status register within region.",   "8" },
        { "max_payload_log",  "Max doorbell payloads to retain for finish().",   "8" })

    GpuTraceRegionHandler(ComponentId_t id, Params& params);

    Action onRead(const QuetzCommand& cmd, QuetzCoreStats& stats) override;
    Action onWrite(const QuetzCommand& cmd, QuetzCoreStats& stats) override;
    void   finish(SST::Output* out, uint32_t core_id) override;

private:
    static uint64_t decodeDoorbellLo(const QuetzCommand& cmd);
    void            recordDoorbellPayload(uint64_t payload);

    uint32_t              doorbell_offset_;
    uint32_t              status_offset_;
    uint32_t              max_payload_log_;
    uint64_t              doorbell_count_;
    uint64_t              poll_count_;
    std::vector<uint64_t> recent_doorbell_lo_;
};

class MmioForwardRegionHandler : public BoundedRegionHandler {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        MmioForwardRegionHandler,
        "quetz",
        "MmioForwardRegionHandler",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Forward MMIO range via mmio_link_N on QuetzCPU (Action::FORWARD_MMIO).",
        SST::Quetz::MemRegionHandler)

    SST_ELI_DOCUMENT_PARAMS(
        { "start", "Inclusive region start address.", "0" },
        { "end",   "Inclusive region end address.",   "0" })

    MmioForwardRegionHandler(ComponentId_t id, Params& params);

    Action onRead(const QuetzCommand& cmd, QuetzCoreStats& stats) override;
    Action onWrite(const QuetzCommand& cmd, QuetzCoreStats& stats) override;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_REGION_HANDLERS
