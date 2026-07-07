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

#ifndef _H_SST_QUETZ_SCALE_OFFSET_KERNEL
#define _H_SST_QUETZ_SCALE_OFFSET_KERNEL

#include "quetz_kernel_api.h"

namespace SST {
namespace Quetz {

// Saturating int16 scale/offset kernel — a sensor-path accelerator
// (calibration / normalization) and the proof that the QuetzKernel API is
// not FFT-shaped. Samples are int16 — little-endian by default, big-endian
// with data_big_endian=1 (pair with the CPU's window_big_endian for BE
// guests); REG_ARG2 = sample count N; REG_ARG3 packs the parameters:
// scale = (int16)(arg3 & 0xFFFF), offset = (int16)((arg3 >> 16) & 0xFFFF).
class ScaleOffsetKernel : public QuetzKernel {
public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        ScaleOffsetKernel,
        "quetz",
        "ScaleOffsetKernel",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Saturating int16 scale/offset kernel (LE s16[N]; N in REG_ARG2, "
        "scale/offset packed in REG_ARG3)",
        SST::Quetz::QuetzKernel)

    SST_ELI_DOCUMENT_PARAMS(
        { "latency_coeff",
          "(uint64) Modeled BUSY cycles = coeff * N. "
          "REG_LATENCY_OVERRIDE still forces an explicit per-op latency.",
          "4" },
        { "data_big_endian",
          "(bool) Interpret the DMA'd buffer as big-endian int16 samples "
          "instead of little-endian. Set alongside the QuetzCPU's "
          "window_big_endian=1 so a big-endian guest's window bytes are "
          "read back as the values it wrote. Default 0 (LE).",
          "0" })

    ScaleOffsetKernel(ComponentId_t id, Params& params);

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

#endif // _H_SST_QUETZ_SCALE_OFFSET_KERNEL
