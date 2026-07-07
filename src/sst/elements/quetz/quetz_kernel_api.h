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

#ifndef _H_SST_QUETZ_KERNEL_API
#define _H_SST_QUETZ_KERNEL_API

#include <sst/core/subcomponent.h>

#include <stdint.h>
#include <string>
#include <vector>

namespace SST {
namespace Quetz {

// Guest-programmed operand registers, captured by QuetzGpuDevice at doorbell
// time (see the REG_ARG* map in quetz_gpu_device.h).
struct KernelArgs {
    uint64_t src_addr;   // REG_ARG0 — input buffer guest-phys address
    uint64_t dst_addr;   // REG_ARG1 — output buffer guest-phys address
    uint64_t arg2;       // REG_ARG2 — kernel-defined (FFT: point count N)
    uint64_t arg3;       // REG_ARG3 — kernel-defined (opcode / packed params)
};

// The compute plugged into QuetzGpuDevice's "kernel" slot. The device owns
// all DMA, doorbell-hold, and BUSY-timing plumbing; a kernel is a pure
// bytes -> bytes + latency function. Keep the math in a standalone header
// (the quetz_fft.h pattern) so it stays host-unit-testable.
class QuetzKernel : public SST::SubComponent {
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(SST::Quetz::QuetzKernel)

    QuetzKernel(ComponentId_t id, Params&) : SubComponent(id) {}
    virtual ~QuetzKernel() {}

    // Bytes to DMA-read from src_addr for this op. Return 0 with `err` set
    // to reject the op — e.g. a non-power-of-two FFT N. The device abandons
    // the op non-fatally: the doorbell completes, kernel_id does not
    // advance, and the rejection is counted in the ops_rejected statistic.
    virtual uint64_t inputBytes(const KernelArgs& args, std::string& err) = 0;

    // Transform `in` -> `out` (the kernel sizes `out`; the device DMA-writes
    // exactly out.size() bytes to dst_addr). Return the modeled latency in
    // device cycles; 0 = no opinion (the device falls back to its
    // kernel_latency param). REG_LATENCY_OVERRIDE beats both.
    virtual uint64_t compute(const KernelArgs& args,
                             const std::vector<uint8_t>& in,
                             std::vector<uint8_t>& out) = 0;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_KERNEL_API
