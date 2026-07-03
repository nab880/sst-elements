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

#include <sst_config.h>
#include "quetz_fft_kernel.h"
#include "quetz_fft.h"

#include <cstring>

using namespace SST;
using namespace SST::Quetz;

namespace {
// LE float32 marshalling — the device's canonical wire format, kept here so
// the device stays ignorant of data formats.
float le32_to_f32(const uint8_t* p) {
    uint32_t b = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
               | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}
void f32_to_le32(float f, uint8_t* p) {
    uint32_t b;
    memcpy(&b, &f, sizeof(b));
    p[0] = (uint8_t)b; p[1] = (uint8_t)(b >> 8);
    p[2] = (uint8_t)(b >> 16); p[3] = (uint8_t)(b >> 24);
}
} // namespace

FFTKernel::FFTKernel(ComponentId_t id, Params& params)
    : QuetzKernel(id, params),
      latency_coeff_(params.find<uint64_t>("fft_latency_coeff", 20))
{}

uint64_t FFTKernel::inputBytes(const KernelArgs& args, std::string& err)
{
    uint64_t n = args.arg2;
    if (n == 0 || (n & (n - 1)) != 0) {
        err = "FFT N (REG_ARG2) must be a nonzero power of two, got "
            + std::to_string(n);
        return 0;
    }
    return n * sizeof(QuetzCf);   /* 8 bytes per complex point */
}

uint64_t FFTKernel::compute(const KernelArgs& args,
                            const std::vector<uint8_t>& in,
                            std::vector<uint8_t>& out)
{
    uint32_t n = (uint32_t)args.arg2;
    uint32_t logn = 0;
    while ((1u << logn) < n) logn++;

    std::vector<QuetzCf> a((size_t)n);
    for (uint32_t i = 0; i < n; i++) {
        a[i].re = le32_to_f32(&in[(size_t)i * 8 + 0]);
        a[i].im = le32_to_f32(&in[(size_t)i * 8 + 4]);
    }
    quetz_fft_radix2(a.data(), n);
    out.resize(in.size());
    for (uint32_t i = 0; i < n; i++) {
        f32_to_le32(a[i].re, &out[(size_t)i * 8 + 0]);
        f32_to_le32(a[i].im, &out[(size_t)i * 8 + 4]);
    }

    return latency_coeff_ * (uint64_t)n * (uint64_t)(logn ? logn : 1);
}
