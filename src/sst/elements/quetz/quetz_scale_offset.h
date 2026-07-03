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

#ifndef _H_SST_QUETZ_SCALE_OFFSET
#define _H_SST_QUETZ_SCALE_OFFSET

// Pure, host-side sample transform used by quetz.ScaleOffsetKernel:
// out = sat16(in * scale + offset), the shape of a sensor-path accelerator
// (calibration / normalization). Header-only and SST-free so the exact math
// the device runs is unit-tested on the host (tests/unit/test_scale_offset.cc)
// — the quetz_fft.h pattern.

#include <cstdint>

namespace SST {
namespace Quetz {

inline int16_t quetz_scale_offset_sat16(int16_t in, int16_t scale, int16_t offset) {
    int32_t v = (int32_t)in * (int32_t)scale + (int32_t)offset;
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_SCALE_OFFSET
