// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "collectiveServiceData.h"

#include <algorithm>
#include <limits>
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

namespace {

bool validStaticValue(const RouteIdV1& route, uint64_t invocation_id,
    const CollectiveSignatureV1& signature, const std::vector<uint8_t>& value)
{
    const auto payload_bytes = signature.payloadBytes();
    return route.valid() && invocation_id != 0 && payload_bytes &&
           *payload_bytes == value.size();
}

} // namespace

bool StaticCollectiveContribution::valid() const
{
    return validStaticValue(route, invocation_id, signature, value);
}

bool StaticCollectiveResult::valid() const
{
    return validStaticValue(route, invocation_id, signature, value);
}

std::optional<uint64_t> staticCollectiveRequestBits(const CollectiveSignatureV1& signature)
{
    if ( signature != STATIC_COLLECTIVE_SIGNATURE_V1 ) return std::nullopt;
    return CollectiveServiceData::MODELED_REQUEST_BITS;
}

std::unique_ptr<SimpleNetwork::Request> makeStaticCollectiveContributionRequest(
    const StaticCollectiveContribution& contribution,
    SimpleNetwork::nid_t destination, SimpleNetwork::nid_t source, int vn)
{
    const auto request_bits = staticCollectiveRequestBits(contribution.signature);
    if ( !contribution.valid() || !request_bits ||
         contribution.value.size() != CollectiveServiceData::VALUE_BYTES ||
         destination < 0 || source < 0 || vn < 0 ||
         *request_bits > std::numeric_limits<size_t>::max() ) {
        return nullptr;
    }

    std::array<uint8_t, CollectiveServiceData::VALUE_BYTES> value {};
    std::copy(contribution.value.begin(), contribution.value.end(), value.begin());
    auto data = std::make_unique<CollectiveServiceData>(contribution.route,
        contribution.invocation_id, CollectiveDirection::Contribution, value);
    if ( !data->validFor(contribution.route, CollectiveDirection::Contribution,
            *request_bits) ) {
        return nullptr;
    }

    auto request = std::make_unique<SimpleNetwork::Request>(
        destination, source, static_cast<size_t>(*request_bits), true, true);
    request->vn = vn;
    request->allow_adaptive = false;
    request->giveServiceData(data.release());
    return request;
}

std::optional<StaticCollectiveResult> inspectStaticCollectiveResult(
    const SimpleNetwork::Request& request, const RouteIdV1& expected_route,
    uint64_t expected_invocation, SimpleNetwork::nid_t expected_source,
    SimpleNetwork::nid_t expected_destination, int expected_vn)
{
    const auto request_bits = staticCollectiveRequestBits(STATIC_COLLECTIVE_SIGNATURE_V1);
    if ( !request_bits || !expected_route.valid() || expected_invocation == 0 ||
         expected_source < 0 || expected_destination < 0 || expected_vn < 0 ||
         request.vn != expected_vn || !request.hasService() ||
         request.getServiceID() != COLLECTIVE_SERVICE_ID ||
         request.src != expected_source || request.dest != expected_destination ||
         request.size_in_bits != *request_bits || !request.head || !request.tail ||
         request.allow_adaptive || request.inspectPayload() != nullptr ) {
        return std::nullopt;
    }

    const auto* data = request.inspectServiceDataAs<CollectiveServiceData>();
    if ( data == nullptr || data->invocation_id != expected_invocation ||
         !data->validFor(expected_route, CollectiveDirection::Result,
             request.size_in_bits) ) {
        return std::nullopt;
    }

    StaticCollectiveResult result;
    result.route = data->route;
    result.invocation_id = data->invocation_id;
    result.signature = STATIC_COLLECTIVE_SIGNATURE_V1;
    result.value.assign(data->value.begin(), data->value.end());
    if ( !result.valid() ) return std::nullopt;
    return result;
}

} // namespace SST::Collective
