// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include "collectiveArithmetic.h"

#include <cstddef>
#include <cstring>
#include <limits>

#ifdef __FAST_MATH__
#error "Collective arithmetic requires strict floating-point semantics"
#endif

namespace SST::Collective {

namespace {

constexpr uint64_t F64_BYTES = 8;

static_assert(sizeof(double) == F64_BYTES, "CollectiveDatatype::F64 requires an eight-byte double");
static_assert(std::numeric_limits<double>::is_iec559, "CollectiveDatatype::F64 requires IEEE-754 arithmetic");
static_assert(std::numeric_limits<double>::radix == 2 && std::numeric_limits<double>::digits == 53,
    "CollectiveDatatype::F64 requires IEEE binary64 precision");

struct AddressRange
{
    uintptr_t begin;
    uintptr_t end;
};

bool
isValidOperation(CollectiveOperation operation) noexcept
{
    switch ( operation ) {
    case CollectiveOperation::Sum:
    case CollectiveOperation::Min:
    case CollectiveOperation::Max:
        return true;
    }
    return false;
}

bool
isValidDatatype(CollectiveDatatype datatype) noexcept
{
    switch ( datatype ) {
    case CollectiveDatatype::I32:
    case CollectiveDatatype::U32:
    case CollectiveDatatype::I64:
    case CollectiveDatatype::U64:
    case CollectiveDatatype::F32:
    case CollectiveDatatype::F64:
        return true;
    }
    return false;
}

bool
makeAddressRange(const void* data, size_t bytes, AddressRange& range) noexcept
{
    if ( data == nullptr ) return false;

    const auto begin = reinterpret_cast<uintptr_t>(data);
    if ( bytes > std::numeric_limits<uintptr_t>::max() - begin ) return false;

    range = { begin, begin + bytes };
    return true;
}

bool
overlaps(const AddressRange& left, const AddressRange& right) noexcept
{
    return left.begin < right.end && right.begin < left.end;
}

} // namespace

ArithmeticStatus
reduceOrdered(CollectiveOperation operation, CollectiveDatatype datatype, uint64_t element_count,
    const BufferView* contributors, uint32_t contributor_count, MutableBufferView result) noexcept
{
    if ( !isValidOperation(operation) || !isValidDatatype(datatype) ) return ArithmeticStatus::Invalid;

    if ( operation != CollectiveOperation::Sum || datatype != CollectiveDatatype::F64 ) {
        return ArithmeticStatus::Unsupported;
    }

    // Zero-count collectives use an endpoint fast path.  Keep this helper
    // total for tests without touching even deliberately invalid pointers.
    if ( element_count == 0 ) return ArithmeticStatus::Success;

    if ( element_count > std::numeric_limits<uint64_t>::max() / F64_BYTES ) return ArithmeticStatus::Invalid;
    const uint64_t expected_bytes_u64 = element_count * F64_BYTES;
    if ( expected_bytes_u64 > std::numeric_limits<size_t>::max() ) return ArithmeticStatus::Invalid;
    const auto expected_bytes = static_cast<size_t>(expected_bytes_u64);

    if ( contributor_count == 0 || contributors == nullptr || result.data == nullptr ||
         result.bytes != expected_bytes_u64 ) {
        return ArithmeticStatus::Invalid;
    }

    if ( static_cast<size_t>(contributor_count) >
         std::numeric_limits<size_t>::max() / sizeof(BufferView) ) {
        return ArithmeticStatus::Invalid;
    }

    AddressRange result_range;
    AddressRange contributor_array_range;
    if ( !makeAddressRange(result.data, expected_bytes, result_range) ||
         !makeAddressRange(contributors, static_cast<size_t>(contributor_count) * sizeof(BufferView),
             contributor_array_range) ||
         overlaps(result_range, contributor_array_range) ) {
        return ArithmeticStatus::Invalid;
    }

    // Complete every metadata and overlap check before changing output.
    for ( uint32_t contributor = 0; contributor < contributor_count; ++contributor ) {
        const BufferView& input = contributors[contributor];
        if ( input.data == nullptr || input.bytes != expected_bytes_u64 ) return ArithmeticStatus::Invalid;

        AddressRange input_range;
        if ( !makeAddressRange(input.data, expected_bytes, input_range) ) return ArithmeticStatus::Invalid;
        if ( input.data != result.data && overlaps(input_range, result_range) ) return ArithmeticStatus::Invalid;
    }

    if ( contributor_count == 1 ) {
        if ( contributors[0].data != result.data ) {
            std::memcpy(result.data, contributors[0].data, expected_bytes);
        }
        return ArithmeticStatus::Success;
    }

    for ( uint64_t element = 0; element < element_count; ++element ) {
        const auto offset = static_cast<size_t>(element) * static_cast<size_t>(F64_BYTES);

        double accumulator;
        std::memcpy(&accumulator, contributors[0].data + offset, sizeof(accumulator));

        for ( uint32_t contributor = 1; contributor < contributor_count; ++contributor ) {
            double operand;
            std::memcpy(&operand, contributors[contributor].data + offset, sizeof(operand));

            // Force one binary64 rounding point per contributor and preserve
            // the caller's deterministic contributor order.
            volatile double rounded = accumulator + operand;
            accumulator             = rounded;
        }

        std::memcpy(result.data + offset, &accumulator, sizeof(accumulator));
    }

    return ArithmeticStatus::Success;
}

} // namespace SST::Collective
