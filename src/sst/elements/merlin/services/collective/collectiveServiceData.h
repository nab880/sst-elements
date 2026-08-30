// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_COLLECTIVE_SERVICE_DATA_H
#define SST_ELEMENTS_COLLECTIVE_SERVICE_DATA_H

#include "collectiveTypes.h"

#include <array>
#include <cstdint>

namespace SST::Collective {

enum class DescriptorValidation : uint8_t {
    Valid = 0,
    InvalidRoute,
    InvalidInvocationId,
    InvalidDirection
};

class CollectiveServiceData final : public SimpleNetwork::NetworkServiceData
{
public:
    static constexpr SimpleNetwork::NetworkServiceID SERVICE_ID = COLLECTIVE_SERVICE_ID;
    static constexpr SimpleNetwork::NetworkServiceDataToken DATA_TOKEN = COLLECTIVE_DATA_TOKEN;
    static constexpr SimpleNetwork::NetworkServiceVersion MIN_SCHEMA_VERSION = COLLECTIVE_SERVICE_SCHEMA_V1;
    static constexpr SimpleNetwork::NetworkServiceVersion MAX_SCHEMA_VERSION = COLLECTIVE_SERVICE_SCHEMA_V1;
    static constexpr uint64_t VALUE_BYTES = 8;
    /** Network timing footprint; checkpoint serialization contains only the fixed fields below. */
    static constexpr uint64_t MODELED_HEADER_BYTES = 90;
    static constexpr uint64_t MODELED_REQUEST_BITS = (MODELED_HEADER_BYTES + VALUE_BYTES) * 8;

    CollectiveServiceData() = default;
    CollectiveServiceData(RouteIdV1 route, uint64_t invocation_id, CollectiveDirection direction,
        std::array<uint8_t, VALUE_BYTES> value);

    SimpleNetwork::NetworkServiceID serviceID() const override { return SERVICE_ID; }
    SimpleNetwork::NetworkServiceDataToken dataToken() const override { return DATA_TOKEN; }
    SimpleNetwork::NetworkServiceVersion schemaVersion() const override { return MIN_SCHEMA_VERSION; }
    CollectiveServiceData* clone() const override;

    DescriptorValidation validateIntrinsic() const;
    bool validFor(const RouteIdV1& expected_route, CollectiveDirection expected_direction,
        uint64_t request_bits) const;

    void serialize_order(SST::Core::Serialization::serializer& ser) override;

    RouteIdV1                        route;
    uint64_t                         invocation_id = 0;
    CollectiveDirection              direction = static_cast<CollectiveDirection>(0);
    std::array<uint8_t, VALUE_BYTES> value {};

private:
    ImplementSerializable(SST::Collective::CollectiveServiceData);
};

static_assert(CollectiveServiceData::VALUE_BYTES == 8);
static_assert(CollectiveServiceData::MODELED_HEADER_BYTES == 90);
static_assert(CollectiveServiceData::MODELED_REQUEST_BITS == 784);

} // namespace SST::Collective

#endif // SST_ELEMENTS_COLLECTIVE_SERVICE_DATA_H
