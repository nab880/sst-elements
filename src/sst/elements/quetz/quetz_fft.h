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

#ifndef _H_SST_QUETZ_FFT
#define _H_SST_QUETZ_FFT

// Pure, host-side radix-2 FFT used by QuetzGpuDevice's kernel_type=fft compute
// path (see quetz_gpu_device.cc). Deliberately header-only and free of any SST
// dependency so the exact math the device runs can be unit-tested on the host
// (tests/unit/test_fft_compute.cc) instead of only through the sim, where an
// impulse input can't validate the twiddle factors.

#include <cmath>
#include <cstdint>

namespace SST {
namespace Quetz {

// Interleaved complex float, matching the device's little-endian float32 wire
// format {re, im} per point.
struct QuetzCf { float re, im; };

// In-place radix-2 decimation-in-time FFT of n complex points; n must be a
// power of two (n == 1 is the identity). Twiddles are evaluated in host double
// for accuracy, then applied in float. Mirrors fft_reference.py.
inline void quetz_fft_radix2(QuetzCf* a, uint32_t n) {
    // M_PI is feature-macro-gated (undefined under strict -std=c++17 on some
    // libc), so define the constant locally — mirrors balar's fft.cu (FFT_PI).
    constexpr double kFftPi = 3.14159265358979323846;

    uint32_t logn = 0;
    while ((1u << logn) < n) logn++;

    // bit-reversal permutation (in place)
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = 0, x = i;
        for (uint32_t b = 0; b < logn; b++) { r = (r << 1) | (x & 1u); x >>= 1; }
        if (r > i) { QuetzCf t = a[i]; a[i] = a[r]; a[r] = t; }
    }

    for (uint32_t s = 1; s <= logn; s++) {
        uint32_t half = 1u << (s - 1);
        for (uint32_t k = 0; k < n / 2u; k++) {
            uint32_t j     = k & (half - 1u);
            uint32_t group = k >> (s - 1u);
            uint32_t i0    = group * (half << 1u) + j;
            uint32_t i1    = i0 + half;
            double   ang   = -2.0 * kFftPi * (double)(j << (logn - s)) / (double)n;
            float    wr    = (float)std::cos(ang), wi = (float)std::sin(ang);
            QuetzCf  v     = a[i1];
            float    tr    = wr * v.re - wi * v.im;
            float    ti    = wr * v.im + wi * v.re;
            QuetzCf  u     = a[i0];
            a[i0].re = u.re + tr; a[i0].im = u.im + ti;
            a[i1].re = u.re - tr; a[i1].im = u.im - ti;
        }
    }
}

} // namespace Quetz
} // namespace SST

#endif
