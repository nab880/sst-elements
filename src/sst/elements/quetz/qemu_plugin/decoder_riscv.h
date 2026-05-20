// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef _QUETZ_DECODER_RISCV_H
#define _QUETZ_DECODER_RISCV_H

#include "../quetz_shmem.h"

namespace SST {
namespace Quetz {

static inline QuetzInsnClass classify_rvc_rv64(const uint32_t enc)
{
    const uint32_t quad = enc & 0x3u;
    const uint32_t fn3  = (enc >> 13) & 0x7u;

    if (quad == 0x0u) {
        switch (fn3) {
        case 0x1u: return QUETZ_INSN_FP_MEM;
        case 0x2u: return QUETZ_INSN_INT_MEM;
        case 0x3u: return QUETZ_INSN_INT_MEM;
        case 0x5u: return QUETZ_INSN_FP_MEM;
        case 0x6u: return QUETZ_INSN_INT_MEM;
        case 0x7u: return QUETZ_INSN_INT_MEM;
        default:   return QUETZ_INSN_INT_COMPUTE;
        }
    }
    if (quad == 0x1u) {
        switch (fn3) {
        case 0x5u: return QUETZ_INSN_BRANCH;
        case 0x6u: return QUETZ_INSN_BRANCH;
        case 0x7u: return QUETZ_INSN_BRANCH;
        default:   return QUETZ_INSN_INT_COMPUTE;
        }
    }
    switch (fn3) {
    case 0x1u: return QUETZ_INSN_FP_MEM;
    case 0x2u: return QUETZ_INSN_INT_MEM;
    case 0x3u: return QUETZ_INSN_INT_MEM;
    case 0x4u: {
        const uint32_t rs2 = (enc >> 2) & 0x1Fu;
        if (rs2 != 0) return QUETZ_INSN_INT_COMPUTE;
        const uint32_t bit12 = (enc >> 12) & 0x1u;
        const uint32_t rd    = (enc >> 7)  & 0x1Fu;
        if (bit12 == 0) return QUETZ_INSN_BRANCH;
        if (rd == 0)    return QUETZ_INSN_OTHER;
        return QUETZ_INSN_BRANCH;
    }
    case 0x5u: return QUETZ_INSN_FP_MEM;
    case 0x6u: return QUETZ_INSN_INT_MEM;
    case 0x7u: return QUETZ_INSN_INT_MEM;
    default:   return QUETZ_INSN_INT_COMPUTE;
    }
}

static inline QuetzInsnClass classify_riscv_insn(const uint32_t enc)
{
    if ((enc & 0x3u) != 0x3u)
        return classify_rvc_rv64(enc);

    const uint32_t opcode = enc & 0x7Fu;
    const uint32_t funct3 = (enc >> 12) & 0x7u;

    switch (opcode) {
    case 0x03u: case 0x23u:
        return QUETZ_INSN_INT_MEM;
    case 0x07u: case 0x27u:
        if (funct3 == 0x2u || funct3 == 0x3u || funct3 == 0x4u)
            return QUETZ_INSN_FP_MEM;
        return QUETZ_INSN_VEC_MEM;
    case 0x2Fu:
        return QUETZ_INSN_INT_MEM;
    case 0x43u: case 0x47u: case 0x4Bu: case 0x4Fu:
        return QUETZ_INSN_FP_COMPUTE;
    case 0x53u:
        return QUETZ_INSN_FP_COMPUTE;
    case 0x57u:
        return QUETZ_INSN_VEC_COMPUTE;
    case 0x63u:
    case 0x67u:
    case 0x6Fu:
        return QUETZ_INSN_BRANCH;
    case 0x13u: case 0x33u:
    case 0x1Bu: case 0x3Bu:
    case 0x17u: case 0x37u:
    case 0x0Fu:
        return QUETZ_INSN_INT_COMPUTE;
    case 0x73u:
        return QUETZ_INSN_OTHER;
    default:
        return QUETZ_INSN_OTHER;
    }
}

} // namespace Quetz
} // namespace SST

#endif // _QUETZ_DECODER_RISCV_H
