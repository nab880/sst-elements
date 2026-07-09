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
// float32 marshalling — LE is the historical wire format; BE mirrors a
// big-endian guest's memory layout (data_big_endian=1). Kept here so the
// device stays ignorant of data formats.
float bytes_to_f32(const uint8_t* p, bool be) {
    uint32_t b = be
        ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
            | ((uint32_t)p[2] << 8) | (uint32_t)p[3]
        : (uint32_t)p[0] | ((uint32_t)p[1] << 8)
            | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}
void f32_to_bytes(float f, uint8_t* p, bool be) {
    uint32_t b;
    memcpy(&b, &f, sizeof(b));
    if (be) {
        p[0] = (uint8_t)(b >> 24); p[1] = (uint8_t)(b >> 16);
        p[2] = (uint8_t)(b >> 8);  p[3] = (uint8_t)b;
    } else {
        p[0] = (uint8_t)b; p[1] = (uint8_t)(b >> 8);
        p[2] = (uint8_t)(b >> 16); p[3] = (uint8_t)(b >> 24);
    }
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
        a[i].re = bytes_to_f32(&in[(size_t)i * 8 + 0], data_big_endian_);
        a[i].im = bytes_to_f32(&in[(size_t)i * 8 + 4], data_big_endian_);
    }
    quetz_fft_radix2(a.data(), n);
    out.resize(in.size());
    for (uint32_t i = 0; i < n; i++) {
        f32_to_bytes(a[i].re, &out[(size_t)i * 8 + 0], data_big_endian_);
        f32_to_bytes(a[i].im, &out[(size_t)i * 8 + 4], data_big_endian_);
    }

    return latency_coeff_ * (uint64_t)n * (uint64_t)(logn ? logn : 1);
}
