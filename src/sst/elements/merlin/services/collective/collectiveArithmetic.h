// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef SST_ELEMENTS_COLLECTIVE_COLLECTIVE_ARITHMETIC_H
#define SST_ELEMENTS_COLLECTIVE_COLLECTIVE_ARITHMETIC_H

#include "collectiveTypes.h"

#include <cstdint>

namespace SST::Collective {

/** Result of a transport-neutral collective arithmetic operation. */
enum class ArithmeticStatus : uint8_t {
    Success     = 1,
    Unsupported = 2,
    Invalid     = 3
};

/**
 * Reduce contributors in their supplied order.
 *
 * PR 1B supports only CollectiveOperation::Sum with
 * CollectiveDatatype::F64.  For a nonzero element count, every input and
 * the output must contain exactly element_count * 8 bytes.  The output may
 * exactly alias any input; every partial input/output overlap is invalid.
 *
 * Unsupported and invalid calls leave the output unchanged.  A supported
 * zero-element call succeeds without inspecting contributors, result, or
 * their pointed-to storage.
 */
[[nodiscard]] ArithmeticStatus reduceOrdered(CollectiveOperation operation, CollectiveDatatype datatype,
    uint64_t element_count, const BufferView* contributors, uint32_t contributor_count,
    MutableBufferView result) noexcept;

} // namespace SST::Collective

#endif // SST_ELEMENTS_COLLECTIVE_COLLECTIVE_ARITHMETIC_H
