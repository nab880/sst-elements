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
#include "quetz_scale_offset_kernel.h"
#include "quetz_scale_offset.h"

using namespace SST;
using namespace SST::Quetz;

ScaleOffsetKernel::ScaleOffsetKernel(ComponentId_t id, Params& params)
    : QuetzKernel(id, params),
      latency_coeff_(params.find<uint64_t>("latency_coeff", 4))
{}

uint64_t ScaleOffsetKernel::inputBytes(const KernelArgs& args, std::string& err)
{
    if (args.arg2 == 0) {
        err = "sample count (REG_ARG2) must be nonzero";
        return 0;
    }
    return args.arg2 * 2;   /* int16 per sample */
}

uint64_t ScaleOffsetKernel::compute(const KernelArgs& args,
                                    const std::vector<uint8_t>& in,
                                    std::vector<uint8_t>& out)
{
    int16_t scale  = (int16_t)(args.arg3 & 0xFFFFu);
    int16_t offset = (int16_t)((args.arg3 >> 16) & 0xFFFFu);

    uint64_t n = args.arg2;
    out.resize(in.size());
    for (uint64_t i = 0; i < n; i++) {
        int16_t s = (int16_t)((uint16_t)in[2 * i] |
                              ((uint16_t)in[2 * i + 1] << 8));
        int16_t r = quetz_scale_offset_sat16(s, scale, offset);
        out[2 * i]     = (uint8_t)((uint16_t)r & 0xFF);
        out[2 * i + 1] = (uint8_t)(((uint16_t)r >> 8) & 0xFF);
    }

    return latency_coeff_ * n;
}
