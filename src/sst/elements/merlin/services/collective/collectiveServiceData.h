// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_COLLECTIVE_SERVICE_DATA_H
#define SST_ELEMENTS_COLLECTIVE_SERVICE_DATA_H

#include "collectiveTypes.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace SST::Collective {

enum class DescriptorValidation : uint8_t {
    Valid = 0,
    InvalidRoute,
    InvalidInvocationId,
    InvalidDirection,
    InvalidSignature,
    InvalidValue
};

/**
 * Wire descriptor for one collective packet.
 *
 * The payload is described by its signature and carried as bytes, so
 * widening the supported operation, datatype, element count, or chunking is
 * a processor/endpoint capability change rather than a schema change.  The
 * static v1 profile accepts exactly STATIC_COLLECTIVE_SIGNATURE_V1 in one
 * chunk; that restriction lives in the processor and endpoints, not here.
 */
class CollectiveServiceData final : public SimpleNetwork::NetworkServiceData
{
public:
    static constexpr SimpleNetwork::NetworkServiceID SERVICE_ID = COLLECTIVE_SERVICE_ID;
    static constexpr SimpleNetwork::NetworkServiceDataToken DATA_TOKEN = COLLECTIVE_DATA_TOKEN;
    static constexpr SimpleNetwork::NetworkServiceVersion MIN_SCHEMA_VERSION = COLLECTIVE_SERVICE_SCHEMA_V1;
    static constexpr SimpleNetwork::NetworkServiceVersion MAX_SCHEMA_VERSION = COLLECTIVE_SERVICE_SCHEMA_V1;

    /** Modeled header bytes charged to every collective packet in addition to its payload. */
    static constexpr uint64_t MODELED_HEADER_BYTES = 90;

    /** Modeled wire size of one packet carrying one chunk of @p signature, or nullopt when invalid. */
    static constexpr std::optional<uint64_t> modeledRequestBits(const CollectiveSignatureV1& signature)
    {
        const std::optional<uint64_t> payload = signature.payloadBytes();
        if ( !payload || *payload > std::numeric_limits<uint64_t>::max() / 8 - MODELED_HEADER_BYTES ) {
            return std::nullopt;
        }
        return (MODELED_HEADER_BYTES + *payload) * 8;
    }

    CollectiveServiceData() = default;
    CollectiveServiceData(RouteIdV1 route, uint64_t invocation_id, CollectiveDirection direction,
        CollectiveSignatureV1 signature, uint32_t chunk_index, std::vector<uint8_t> value) :
        route(route),
        invocation_id(invocation_id),
        direction(direction),
        signature(signature),
        chunk_index(chunk_index),
        value(std::move(value))
    {}

    SimpleNetwork::NetworkServiceID serviceID() const override { return SERVICE_ID; }
    SimpleNetwork::NetworkServiceDataToken dataToken() const override { return DATA_TOKEN; }
    SimpleNetwork::NetworkServiceVersion schemaVersion() const override { return MIN_SCHEMA_VERSION; }

    CollectiveServiceData* clone() const override
    {
        if ( validateIntrinsic() != DescriptorValidation::Valid ) {
            throw std::logic_error("Cannot clone malformed collective service data");
        }
        return new CollectiveServiceData(*this);
    }

    DescriptorValidation validateIntrinsic() const
    {
        if ( !route.valid() ) return DescriptorValidation::InvalidRoute;
        if ( invocation_id == 0 ) return DescriptorValidation::InvalidInvocationId;
        if ( !isValid(direction) ) return DescriptorValidation::InvalidDirection;
        const std::optional<uint64_t> payload = signature.payloadBytes();
        if ( !payload ) return DescriptorValidation::InvalidSignature;
        if ( value.size() != *payload ) return DescriptorValidation::InvalidValue;
        return DescriptorValidation::Valid;
    }

    bool validFor(const RouteIdV1& expected_route, CollectiveDirection expected_direction,
        uint64_t request_bits) const
    {
        return validateIntrinsic() == DescriptorValidation::Valid && route == expected_route &&
               direction == expected_direction && modeledRequestBits(signature) == request_bits;
    }

    void serialize_order(SST::Core::Serialization::serializer& ser) override
    {
        if ( ser.mode() != SST::Core::Serialization::serializer::UNPACK &&
             validateIntrinsic() != DescriptorValidation::Valid ) {
            throw std::logic_error("Cannot serialize malformed collective service data");
        }

        SST_SER(route);
        SST_SER(invocation_id);
        SST_SER(direction);
        SST_SER(signature);
        SST_SER(chunk_index);
        SST_SER(value);

        if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK &&
             validateIntrinsic() != DescriptorValidation::Valid ) {
            throw std::runtime_error("Malformed serialized collective service data");
        }
    }

    RouteIdV1             route;
    uint64_t              invocation_id = 0;
    CollectiveDirection   direction = static_cast<CollectiveDirection>(0);
    CollectiveSignatureV1 signature;
    uint32_t              chunk_index = 0;
    std::vector<uint8_t>  value;

private:
    ImplementSerializable(SST::Collective::CollectiveServiceData);
};

/** The one profile the static v1 processor and endpoints accept: one F64 SUM element in one chunk. */
inline constexpr CollectiveSignatureV1 STATIC_COLLECTIVE_SIGNATURE_V1 {
    CollectiveOperation::Sum, CollectiveDatatype::F64, 1 };
inline constexpr uint64_t STATIC_COLLECTIVE_REQUEST_BITS = 784;

static_assert(CollectiveServiceData::MODELED_HEADER_BYTES == 90);
static_assert(CollectiveServiceData::modeledRequestBits(STATIC_COLLECTIVE_SIGNATURE_V1) ==
              STATIC_COLLECTIVE_REQUEST_BITS);

/** Native-stack-neutral contribution handed to a static transport adapter. */
struct StaticCollectiveContribution
{
    RouteIdV1             route;
    uint64_t              invocation_id = 0;
    CollectiveSignatureV1 signature;
    std::vector<uint8_t>  value;

    bool valid() const
    {
        const std::optional<uint64_t> payload_bytes = signature.payloadBytes();
        return route.valid() && invocation_id != 0 && payload_bytes && *payload_bytes == value.size();
    }
};

/** Validated result returned by a static transport adapter. */
struct StaticCollectiveResult
{
    RouteIdV1             route;
    uint64_t              invocation_id = 0;
    CollectiveSignatureV1 signature;
    std::vector<uint8_t>  value;

    bool valid() const
    {
        const std::optional<uint64_t> payload_bytes = signature.payloadBytes();
        return route.valid() && invocation_id != 0 && payload_bytes && *payload_bytes == value.size();
    }
};

/** Returns the atomic wire footprint for a signature supported by the static v1 profile. */
inline std::optional<uint64_t> staticCollectiveRequestBits(const CollectiveSignatureV1& signature)
{
    if ( signature != STATIC_COLLECTIVE_SIGNATURE_V1 ) return std::nullopt;
    return STATIC_COLLECTIVE_REQUEST_BITS;
}

/** Centralized v1 packet construction; adapters own only transport scheduling. */
inline std::unique_ptr<SimpleNetwork::Request> makeStaticCollectiveContributionRequest(
    const StaticCollectiveContribution& contribution,
    SimpleNetwork::nid_t destination, SimpleNetwork::nid_t source, int vn)
{
    const std::optional<uint64_t> request_bits = staticCollectiveRequestBits(contribution.signature);
    if ( !contribution.valid() || !request_bits || destination < 0 || source < 0 || vn < 0 ||
         *request_bits > std::numeric_limits<size_t>::max() ) {
        return nullptr;
    }

    auto data = std::make_unique<CollectiveServiceData>(contribution.route,
        contribution.invocation_id, CollectiveDirection::Contribution, contribution.signature, 0,
        contribution.value);
    if ( !data->validFor(contribution.route, CollectiveDirection::Contribution, *request_bits) ) {
        return nullptr;
    }

    auto request = std::make_unique<SimpleNetwork::Request>(
        destination, source, static_cast<size_t>(*request_bits), true, true);
    request->vn = vn;
    request->allow_adaptive = false;
    request->giveServiceData(data.release());
    return request;
}

/** Centralized v1 result validation and decoding. */
inline std::optional<StaticCollectiveResult> inspectStaticCollectiveResult(
    const SimpleNetwork::Request& request, const RouteIdV1& expected_route,
    uint64_t expected_invocation, SimpleNetwork::nid_t expected_source,
    SimpleNetwork::nid_t expected_destination, int expected_vn)
{
    if ( !expected_route.valid() || expected_invocation == 0 || expected_source < 0 ||
         expected_destination < 0 || expected_vn < 0 || request.vn != expected_vn ||
         !request.hasService() || request.getServiceID() != COLLECTIVE_SERVICE_ID ||
         request.src != expected_source || request.dest != expected_destination ||
         request.size_in_bits != STATIC_COLLECTIVE_REQUEST_BITS || !request.head || !request.tail ||
         request.allow_adaptive || request.inspectPayload() != nullptr ) {
        return std::nullopt;
    }

    const CollectiveServiceData* data = request.inspectServiceDataAs<CollectiveServiceData>();
    if ( data == nullptr || data->invocation_id != expected_invocation ||
         data->signature != STATIC_COLLECTIVE_SIGNATURE_V1 || data->chunk_index != 0 ||
         !data->validFor(expected_route, CollectiveDirection::Result, request.size_in_bits) ) {
        return std::nullopt;
    }

    StaticCollectiveResult result;
    result.route         = data->route;
    result.invocation_id = data->invocation_id;
    result.signature     = data->signature;
    result.value         = data->value;
    if ( !result.valid() ) return std::nullopt;
    return result;
}

} // namespace SST::Collective

#endif // SST_ELEMENTS_COLLECTIVE_SERVICE_DATA_H
