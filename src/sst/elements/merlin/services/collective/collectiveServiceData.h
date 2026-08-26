// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_COLLECTIVE_SERVICE_DATA_H
#define SST_ELEMENTS_COLLECTIVE_SERVICE_DATA_H

#include "collectiveTypes.h"

#include <cstdint>
#include <vector>

namespace SST::Collective {

enum class DescriptorValidation : uint8_t {
    Valid = 0,
    InvalidServiceSchema,
    InvalidWireFormat,
    InvalidNamespace,
    InvalidChunkCount,
    InvalidChunkIndex,
    InvalidElementCount,
    InvalidElementRange,
    InvalidOperation,
    InvalidDatatype,
    InvalidArithmeticPolicy,
    InvalidDirection,
    PayloadSizeOverflow,
    PayloadSizeMismatch,
    InvalidDataPresent,
    OwnedSizeMismatch,
    InvalidModeledSize,
    EncodedLengthMismatch,
    EncodedLengthTooLarge
};

struct CollectiveDescriptorFieldsV1
{
    uint16_t             service_schema_version = COLLECTIVE_SERVICE_SCHEMA_V1;
    uint16_t             wire_format_version    = COLLECTIVE_WIRE_FORMAT_V1;
    RouteIdV1            route;
    uint64_t             invocation_id           = 0;
    uint32_t             chunk_index             = 0;
    uint32_t             total_chunks            = 0;
    uint64_t             element_offset          = 0;
    uint64_t             element_count           = 0;
    uint64_t             total_elements          = 0;
    CollectiveOperation  operation               = static_cast<CollectiveOperation>(0);
    CollectiveDatatype   datatype                = static_cast<CollectiveDatatype>(0);
    uint16_t             arithmetic_policy       = COLLECTIVE_ARITHMETIC_POLICY_V1;
    CollectiveDirection  direction               = static_cast<CollectiveDirection>(0);
    uint64_t             logical_payload_bytes   = 0;
    uint64_t             modeled_wire_bytes      = 0;
    uint8_t              data_present            = 0;
};

class CollectiveServiceData final : public SimpleNetwork::NetworkServiceData
{
public:
    static constexpr SimpleNetwork::NetworkServiceID SERVICE_ID = COLLECTIVE_SERVICE_ID;
    static constexpr SimpleNetwork::NetworkServiceDataToken DATA_TOKEN = COLLECTIVE_DATA_TOKEN;
    static constexpr SimpleNetwork::NetworkServiceVersion MIN_SCHEMA_VERSION = COLLECTIVE_SERVICE_SCHEMA_V1;
    static constexpr SimpleNetwork::NetworkServiceVersion MAX_SCHEMA_VERSION = COLLECTIVE_SERVICE_SCHEMA_V1;
    static constexpr uint64_t FIXED_PREFIX_BYTES = 90;
    static constexpr uint64_t MAX_OWNED_BYTES = UINT32_MAX;

    CollectiveServiceData() = default;
    CollectiveServiceData(CollectiveDescriptorFieldsV1 fields, std::vector<uint8_t> owned_bytes);
    ~CollectiveServiceData() override;

    SimpleNetwork::NetworkServiceID serviceID() const override { return SERVICE_ID; }
    SimpleNetwork::NetworkServiceDataToken dataToken() const override { return DATA_TOKEN; }
    SimpleNetwork::NetworkServiceVersion schemaVersion() const override
    {
        return fields.service_schema_version;
    }
    CollectiveServiceData* clone() const override;

    DescriptorValidation validateIntrinsic() const;

    std::vector<uint8_t> canonicalBytes() const;
    static DescriptorValidation decodeCanonical(
        const uint8_t* bytes, uint64_t length, CollectiveServiceData& output);

    void serialize_order(SST::Core::Serialization::serializer& ser) override;

    CollectiveDescriptorFieldsV1 fields;
    std::vector<uint8_t>          owned_bytes;

private:
    ImplementSerializable(SST::Collective::CollectiveServiceData);
};

} // namespace SST::Collective

#endif // SST_ELEMENTS_COLLECTIVE_SERVICE_DATA_H
