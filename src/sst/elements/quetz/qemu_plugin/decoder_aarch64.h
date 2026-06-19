// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef _QUETZ_DECODER_AARCH64_H
#define _QUETZ_DECODER_AARCH64_H

#include "../quetz_shmem.h"

namespace SST {
namespace Quetz {

static inline QuetzInsnClass classify_aarch64_insn(const uint32_t enc)
{
    if ((enc & (1u << 27u)) && !(enc & (1u << 25u)))
        return (enc & (1u << 26u)) ? QUETZ_INSN_VEC_MEM : QUETZ_INSN_INT_MEM;

    const uint32_t grp = (enc >> 25u) & 0xFu;

    switch (grp) {
    case 0x8u: case 0x9u:
        return QUETZ_INSN_INT_COMPUTE;
    case 0x5u: case 0xDu:
        return QUETZ_INSN_INT_COMPUTE;
    case 0x7u:
        return QUETZ_INSN_VEC_COMPUTE;
    case 0xEu: case 0xFu:
        return QUETZ_INSN_FP_COMPUTE;
    case 0x2u: case 0x3u: case 0xAu: case 0xBu:
        return QUETZ_INSN_BRANCH;
    case 0x1u:
        return QUETZ_INSN_VEC_COMPUTE;
    case 0x0u:
    default:
        return QUETZ_INSN_OTHER;
    }
}

} // namespace Quetz
} // namespace SST

#endif // _QUETZ_DECODER_AARCH64_H
