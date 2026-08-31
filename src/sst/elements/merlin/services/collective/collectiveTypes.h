// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_COLLECTIVE_TYPES_H
#define SST_ELEMENTS_COLLECTIVE_TYPES_H

#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/serialization/serializer.h>

#include <cstdint>
#include <limits>
#include <optional>

namespace SST::Collective {

using SimpleNetwork = SST::Interfaces::SimpleNetwork;

inline constexpr SimpleNetwork::NetworkServiceID COLLECTIVE_SERVICE_ID = 1;
inline constexpr SimpleNetwork::NetworkServiceDataToken COLLECTIVE_DATA_TOKEN = 1;
inline constexpr SimpleNetwork::NetworkServiceVersion COLLECTIVE_SERVICE_SCHEMA_V1 = 1;
inline constexpr uint16_t COLLECTIVE_RUNTIME_SCHEMA_V1 = 1;

enum class CollectiveOperation : uint8_t { Sum = 1, Min = 2, Max = 3 };
enum class CollectiveDatatype : uint8_t { I32 = 1, U32 = 2, I64 = 3, U64 = 4, F32 = 5, F64 = 6 };
enum class CollectiveDirection : uint8_t { Contribution = 1, Result = 2 };
enum class CollectiveRouteKind : uint8_t { EndpointLocal = 1, FabricTree = 2 };
enum class CollectiveDataMode : uint8_t { Functional = 1, TimingOnly = 2 };

inline constexpr bool isValid(CollectiveOperation value)
{
    return value == CollectiveOperation::Sum || value == CollectiveOperation::Min ||
           value == CollectiveOperation::Max;
}

inline constexpr bool isValid(CollectiveDatatype value)
{
    return value >= CollectiveDatatype::I32 && value <= CollectiveDatatype::F64;
}

inline constexpr bool isValid(CollectiveDirection value)
{
    return value == CollectiveDirection::Contribution || value == CollectiveDirection::Result;
}

inline constexpr bool isValid(CollectiveRouteKind value)
{
    return value == CollectiveRouteKind::EndpointLocal || value == CollectiveRouteKind::FabricTree;
}

inline constexpr bool isValid(CollectiveDataMode value)
{
    return value == CollectiveDataMode::Functional || value == CollectiveDataMode::TimingOnly;
}

inline constexpr uint64_t collectiveDatatypeBytes(CollectiveDatatype datatype)
{
    switch ( datatype ) {
    case CollectiveDatatype::I32:
    case CollectiveDatatype::U32:
    case CollectiveDatatype::F32:
        return 4;
    case CollectiveDatatype::I64:
    case CollectiveDatatype::U64:
    case CollectiveDatatype::F64:
        return 8;
    }
    return 0;
}

/** Native-stack-neutral description of one collective payload. */
struct CollectiveSignatureV1
{
    CollectiveOperation operation = static_cast<CollectiveOperation>(0);
    CollectiveDatatype  datatype  = static_cast<CollectiveDatatype>(0);
    uint64_t            element_count = 0;

    constexpr bool valid() const
    {
        const uint64_t width = collectiveDatatypeBytes(datatype);
        return isValid(operation) && width != 0 && element_count != 0 &&
               element_count <= std::numeric_limits<uint64_t>::max() / width;
    }

    std::optional<uint64_t> payloadBytes() const
    {
        if ( !valid() ) return std::nullopt;
        return element_count * collectiveDatatypeBytes(datatype);
    }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(operation);
        SST_SER(datatype);
        SST_SER(element_count);
    }
};

inline constexpr bool operator==(const CollectiveSignatureV1& lhs, const CollectiveSignatureV1& rhs)
{
    return lhs.operation == rhs.operation && lhs.datatype == rhs.datatype &&
           lhs.element_count == rhs.element_count;
}

inline constexpr bool operator!=(const CollectiveSignatureV1& lhs, const CollectiveSignatureV1& rhs)
{
    return !(lhs == rhs);
}

struct RouteIdV1
{
    uint64_t job_namespace = 0;
    uint64_t route_id      = 0;

    constexpr bool valid() const { return job_namespace != 0; }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(job_namespace);
        SST_SER(route_id);
    }
};

inline constexpr bool operator==(const RouteIdV1& lhs, const RouteIdV1& rhs)
{
    return lhs.job_namespace == rhs.job_namespace && lhs.route_id == rhs.route_id;
}

inline constexpr bool operator!=(const RouteIdV1& lhs, const RouteIdV1& rhs) { return !(lhs == rhs); }

struct BufferView
{
    const uint8_t* data  = nullptr;
    uint64_t       bytes = 0;
};

struct MutableBufferView
{
    uint8_t* data  = nullptr;
    uint64_t bytes = 0;
};

} // namespace SST::Collective

#endif // SST_ELEMENTS_COLLECTIVE_TYPES_H
