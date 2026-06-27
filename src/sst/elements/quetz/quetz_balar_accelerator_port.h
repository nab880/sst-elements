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

#ifndef _H_SST_QUETZ_BALAR_ACCELERATOR_PORT
#define _H_SST_QUETZ_BALAR_ACCELERATOR_PORT

#include <sst/core/output.h>

#include <cstdint>
#include <deque>
#include <unordered_map>

#include "quetz_accelerator_port.h"

namespace SST {
namespace Quetz {

// The balar host-side port. Encapsulates the doorbell coherence bridge (flush
// the staged-packet range before forwarding the doorbell so balar DMAs coherent
// bytes) and the P4 posted/async completion engine (submit aperture, ticket /
// COMPLETED counters, completion queue, sim-hold). Behaviorally identical to the
// logic that previously lived in quetzcpu_mmio_sync.cc.
class BalarAcceleratorPort : public AcceleratorPort {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        BalarAcceleratorPort,
        "quetz",
        "BalarAcceleratorPort",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Host-side accelerator port for balar: doorbell aperture, "
        "flush-before-doorbell coherence policy, and the posted/async "
        "completion engine.",
        SST::Quetz::AcceleratorPort)

    SST_ELI_DOCUMENT_PARAMS(
        { "verbose", "Verbosity level.", "0" },
        { "doorbell_addr",
          "MMIO address of the doorbell. Synchronous writes in this range flush "
          "the staged-packet range on the cache link before forwarding the "
          "doorbell on the mmio link.", "0" },
        { "doorbell_size", "Size in bytes of the doorbell aperture.", "8" },
        { "flush_mode",
          "Coherence-flush policy applied before forwarding a doorbell: "
          "'range_from_value' (flush packet_flush_bytes from the staged-packet "
          "address carried in the doorbell write — the balar coherence bridge) "
          "or 'none' (forward without flushing).", "range_from_value" },
        { "packet_flush_bytes",
          "Bytes to FlushAddr(inv) from the staged-packet (scratch) address "
          "before forwarding a doorbell write (0 = no flush).", "4096" },
        { "async_offload",
          "If 1, expose the async submit/completion aperture (posted offload).",
          "0" },
        { "async_doorbell_addr",
          "Base MMIO address of the async submit/completion aperture.", "0" },
        { "async_doorbell_size",
          "Size in bytes of the async submit/completion aperture.", "0x40" },
        { "async_completion_depth",
          "Maximum number of posted offloads in flight per vCPU.", "1" })

    BalarAcceleratorPort(ComponentId_t id, Params& params, AcceleratorHost* host);
    ~BalarAcceleratorPort() {}

    bool ownsAddr(uint64_t addr) const override;
    void handleCommand(uint32_t vcpu, const QuetzCommand& cmd) override;
    bool handleResponse(uint32_t vcpu,
                        SST::Interfaces::StandardMem::Request* resp) override;
    void process() override;
    bool hasOutstanding() const override;
    bool vcpuHasOutstanding(uint32_t vcpu) const override;

private:
    void handleAsyncAperture(uint32_t vcpu, const QuetzCommand& cmd);
    void issueDoorbellFlushes(uint32_t vcpu,
                              SST::Interfaces::StandardMem::Write* req,
                              bool is_async, bool pre_acked);
    void forwardDoorbell(uint32_t vcpu, SST::Interfaces::StandardMem::Write* req,
                         bool is_async, bool pre_acked);

    struct ForwardPending {
        uint32_t vcpu;
        bool     is_read;
        bool     is_async;
    };
    struct FlushCtx {
        uint32_t vcpu;
        uint32_t remaining;
        uint64_t start_cycle;
        SST::Interfaces::StandardMem::Write* doorbell;
        bool     is_async;
        bool     pre_acked;
    };
    struct ArmedDoorbell {
        SST::Interfaces::StandardMem::Write* req;
        bool                                 is_async;
        bool                                 pre_acked;
    };

    SST::Output      out_;
    AcceleratorHost* host_;

    uint64_t doorbell_addr_;
    uint64_t doorbell_size_;
    uint64_t packet_flush_bytes_;
    bool     flush_enabled_;   // flush_mode == range_from_value (vs none)
    bool     async_offload_;
    uint64_t async_doorbell_addr_;
    uint64_t async_doorbell_size_;
    uint32_t async_completion_depth_;

    std::unordered_map<uint64_t, ForwardPending> pending_;
    std::unordered_map<uint64_t, uint32_t>       flush_to_vcpu_;
    std::unordered_map<uint32_t, FlushCtx>       doorbell_flushes_;
    std::unordered_map<uint32_t, ArmedDoorbell>  armed_doorbells_;
    std::unordered_map<uint32_t, uint64_t> submit_id_;
    std::unordered_map<uint32_t, uint64_t> completed_id_;
    std::unordered_map<uint32_t, uint64_t> last_result_;
    std::unordered_map<uint32_t, bool>     async_busy_;
    std::unordered_map<uint32_t, std::deque<SST::Interfaces::StandardMem::Write*>>
                                           submit_queue_;
    uint32_t async_in_flight_ = 0;
    // async_in_flight_ and async_completion_depth_ are global, not per-vCPU, so
    // the async engine currently assumes a single vCPU drives it; enforced in
    // handleAsyncAperture(). Make these per-vCPU before multi-vCPU async offload.
    int      async_vcpu_ = -1;

    // Async submit-aperture register offsets (layout matches QuetzGpuDevice).
    static constexpr uint64_t ASYNC_REG_SUBMIT    = 0x00;
    static constexpr uint64_t ASYNC_REG_STATUS    = 0x08;
    static constexpr uint64_t ASYNC_REG_COMPLETED = 0x10;
    static constexpr uint64_t ASYNC_REG_TICKET    = 0x20;
    static constexpr uint64_t ASYNC_REG_RESULT    = 0x28;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_BALAR_ACCELERATOR_PORT
