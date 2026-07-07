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

#ifndef _H_SST_QUETZ_FFT_KERNEL
#define _H_SST_QUETZ_FFT_KERNEL

#include "quetz_kernel_api.h"

namespace SST {
namespace Quetz {

// Radix-2 FFT kernel for QuetzGpuDevice's "kernel" slot. Buffers are
// float32 cfloat[N] {re, im} — little-endian by default, big-endian with
// data_big_endian=1 (pair it with the CPU's window_big_endian for BE
// guests); N comes from REG_ARG2 and must be a nonzero power of two. The
// math lives in quetz_fft.h (host-side, unit-tested by
// tests/unit/test_fft_compute.cc); this class only marshals and models
// latency.
class FFTKernel : public QuetzKernel {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        FFTKernel,
        "quetz",
        "FFTKernel",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Radix-2 FFT compute kernel (float32 cfloat[N]; N in REG_ARG2)",
        SST::Quetz::QuetzKernel)

    SST_ELI_DOCUMENT_PARAMS(
        { "fft_latency_coeff",
          "(uint64) Modeled BUSY cycles = coeff * N * log2(N). "
          "REG_LATENCY_OVERRIDE still forces an explicit per-op latency.",
          "20" },
        { "data_big_endian",
          "(bool) Interpret the DMA'd buffer as big-endian float32 words "
          "instead of little-endian. Set alongside the QuetzCPU's "
          "window_big_endian=1 so a big-endian guest's window bytes are "
          "read back as the values it wrote. Default 0 (LE).",
          "0" })

    FFTKernel(ComponentId_t id, Params& params);

    uint64_t inputBytes(const KernelArgs& args, std::string& err) override;
    uint64_t compute(const KernelArgs& args,
                     const std::vector<uint8_t>& in,
                     std::vector<uint8_t>& out) override;

private:
    uint64_t latency_coeff_;
    bool     big_endian_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_FFT_KERNEL
