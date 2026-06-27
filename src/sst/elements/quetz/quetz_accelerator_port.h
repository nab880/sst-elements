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

#ifndef _H_SST_QUETZ_ACCELERATOR_PORT
#define _H_SST_QUETZ_ACCELERATOR_PORT

#include <sst/core/interfaces/stdMem.h>
#include <sst/core/subcomponent.h>

#include <cstdint>
#include <vector>

#include "quetz_ipc_types.h"

namespace SST {
namespace Quetz {

// Little-endian (de)serialization shared by the generic MMIO-sync path and the
// accelerator ports. The shared-memory mailbox already delivers write payloads
// in LE byte order (see pollMmioSyncMailbox), so these match it.
inline uint64_t accelDataToU64(const std::vector<uint8_t>& data) {
    uint64_t val = 0;
    for (int i = (int)data.size() - 1; i >= 0; i--) {
        val <<= 8;
        val |= data[(size_t)i];
    }
    return val;
}

inline std::vector<uint8_t> accelU64ToData(uint64_t val, size_t size) {
    std::vector<uint8_t> out(size, 0);
    for (size_t i = 0; i < size; i++) {
        out[i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
    return out;
}

// Narrow services an AcceleratorPort calls back into, implemented by QuetzCPU.
// Keeps the port free of any dependency on the CPU model's internals — it sees
// only per-vCPU links, the guest-unblock primitive, drain state, timing, and the
// per-core statistic recorders.
struct AcceleratorHost {
    virtual ~AcceleratorHost() {}

    // Unblock the guest vCPU waiting on its synchronous MMIO request.
    virtual void postResponse(uint32_t vcpu, uint64_t value) = 0;
    // Issue on the vCPU's cache_link (coherence flushes) / mmio_link (doorbell).
    virtual void sendMem (uint32_t vcpu, SST::Interfaces::StandardMem::Request* r) = 0;
    virtual void sendMmio(uint32_t vcpu, SST::Interfaces::StandardMem::Request* r) = 0;

    virtual bool     isDrained(uint32_t vcpu) const = 0;
    virtual bool     hasMem(uint32_t vcpu)  const = 0;
    virtual bool     hasMmio(uint32_t vcpu) const = 0;
    virtual uint64_t cacheLineSize() const = 0;
    virtual uint64_t cycles() const = 0;

    virtual void recordSyncRequest(uint32_t vcpu, bool is_read) = 0;
    virtual void recordDoorbellFlush(uint32_t vcpu) = 0;
    virtual void recordDoorbellFlushCycles(uint32_t vcpu, uint64_t cycles) = 0;
    virtual void recordAsyncSubmit(uint32_t vcpu) = 0;
    virtual void recordAsyncCompletion(uint32_t vcpu) = 0;
};

// A first-class accelerator host-side port: owns one accelerator's doorbell
// aperture, its coherence-flush policy, and (optionally) the posted/async
// completion engine. QuetzCPU routes synchronous MMIO commands and memory-system
// responses to it and ticks it; the port never touches CPU internals directly.
class AcceleratorPort : public SST::SubComponent {
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(SST::Quetz::AcceleratorPort,
                                      SST::Quetz::AcceleratorHost*)

    AcceleratorPort(ComponentId_t id, Params& params)
        : SST::SubComponent(id)
    {}

    // True if `addr` falls in one of this port's apertures (doorbell or async).
    virtual bool ownsAddr(uint64_t addr) const = 0;
    // Service a synchronous MMIO read/write the mailbox routed here; the port is
    // responsible for unblocking the guest (now or on later completion).
    virtual void handleCommand(uint32_t vcpu, const QuetzCommand& cmd) = 0;
    // Offer a memory-system response; return true if it was one of this port's
    // (a flush or a forwarded doorbell), false to let the generic path keep it.
    virtual bool handleResponse(uint32_t vcpu,
                                SST::Interfaces::StandardMem::Request* resp) = 0;
    // Per-CPU-tick hook: advance deferred doorbells / queued submits.
    virtual void process() = 0;
    // True while any posted op is outstanding (keeps the simulation alive).
    virtual bool hasOutstanding() const = 0;
    // True if vCPU `vcpu` has a posted offload not yet retired (overlap metric).
    virtual bool vcpuHasOutstanding(uint32_t vcpu) const = 0;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_ACCELERATOR_PORT
