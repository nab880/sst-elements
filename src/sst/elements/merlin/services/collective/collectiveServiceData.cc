// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "collectiveServiceData.h"

#include <stdexcept>

namespace SST::Collective {

CollectiveServiceData::CollectiveServiceData(RouteIdV1 route, uint64_t invocation_id,
    CollectiveDirection direction, std::array<uint8_t, VALUE_BYTES> value) :
    route(route),
    invocation_id(invocation_id),
    direction(direction),
    value(value)
{}

CollectiveServiceData* CollectiveServiceData::clone() const
{
    if ( validateIntrinsic() != DescriptorValidation::Valid ) {
        throw std::logic_error("Cannot clone malformed collective service data");
    }
    return new CollectiveServiceData(*this);
}

DescriptorValidation CollectiveServiceData::validateIntrinsic() const
{
    if ( !route.valid() ) return DescriptorValidation::InvalidRoute;
    if ( invocation_id == 0 ) return DescriptorValidation::InvalidInvocationId;
    if ( !isValid(direction) ) return DescriptorValidation::InvalidDirection;
    return DescriptorValidation::Valid;
}

bool CollectiveServiceData::validFor(const RouteIdV1& expected_route,
    CollectiveDirection expected_direction, uint64_t request_bits) const
{
    return validateIntrinsic() == DescriptorValidation::Valid && route == expected_route &&
           direction == expected_direction && request_bits == MODELED_REQUEST_BITS;
}

void CollectiveServiceData::serialize_order(SST::Core::Serialization::serializer& ser)
{
    if ( ser.mode() != SST::Core::Serialization::serializer::UNPACK &&
         validateIntrinsic() != DescriptorValidation::Valid ) {
        throw std::logic_error("Cannot serialize malformed collective service data");
    }

    SST_SER(route);
    SST_SER(invocation_id);
    SST_SER(direction);
    SST_SER(value);

    if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK &&
         validateIntrinsic() != DescriptorValidation::Valid ) {
        throw std::runtime_error("Malformed serialized collective service data");
    }
}

} // namespace SST::Collective
