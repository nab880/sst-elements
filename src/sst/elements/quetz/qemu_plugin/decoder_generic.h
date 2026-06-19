// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef _QUETZ_DECODER_GENERIC_H
#define _QUETZ_DECODER_GENERIC_H

#include "../quetz_shmem.h"

namespace SST {
namespace Quetz {

static inline QuetzInsnClass classify_by_size(const uint32_t size)
{
    return (size >= 16) ? QUETZ_INSN_VEC_MEM : QUETZ_INSN_INT_MEM;
}

} // namespace Quetz
} // namespace SST

#endif // _QUETZ_DECODER_GENERIC_H
