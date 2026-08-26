// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_COLLECTIVE_ROUTE_H
#define SST_ELEMENTS_COLLECTIVE_ROUTE_H

#include "collectiveEndpoint.h"
#include "collectiveServiceData.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace SST::Collective {

enum class CollectiveRouteValidation : uint8_t {
    Valid = 0,
    InvalidRuntimeSchema,
    InvalidProjectionSchema,
    InvalidRoute,
    InvalidRouteKind,
    InvalidDataMode,
    InvalidOperationMask,
    InvalidDatatypeMask,
    InvalidDescriptorSchema,
    InvalidArithmeticPolicy,
    InvalidQuota,
    InvalidWindow,
    InvalidChunkLimit,
    InvalidFabricConfiguration,
    RouteMismatch,
    RouteKindMismatch,
    InvalidEndpoint,
    InvalidParticipantCount,
    InvalidParticipant,
    InvalidBinding,
    InvalidRepresentative,
    InvalidParent,
    InvalidPort,
    DuplicatePort,
    EmptyIngress
};

enum class CollectiveRouteInstallResult : uint8_t { Installed = 1, Unsupported = 2, Invalid = 3 };

enum class CollectiveIngressRole : uint8_t {
    LocalEndpointContribution = 1,
    ChildContribution         = 2,
    ParentResult              = 3
};

enum class CollectivePacketValidation : uint8_t {
    Valid = 0,
    MalformedDescriptor,
    InvalidRuntime,
    EndpointLocalHasNoPacket,
    RouteMismatch,
    UnsupportedOperation,
    UnsupportedDatatype,
    SchemaMismatch,
    ArithmeticPolicyMismatch,
    DirectionMismatch,
    DataModeMismatch,
    InvalidChunkLayout,
    ModeledSizeOverflow,
    ModeledSizeMismatch,
    RequestSizeMismatch
};

inline constexpr bool isValid(CollectiveRouteInstallResult value)
{
    return value >= CollectiveRouteInstallResult::Installed && value <= CollectiveRouteInstallResult::Invalid;
}

namespace detail {

template <class T>
void serializeRouteVector(SST::Core::Serialization::serializer& ser, std::vector<T>& values)
{
    uint64_t count = static_cast<uint64_t>(values.size());
    SST_SER(count);

    if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
        if ( count > UINT32_MAX ) throw std::range_error("Collective route projection vector is too large");
        std::vector<T> decoded;
        for ( uint64_t i = 0; i < count; ++i ) {
            T value;
            SST_SER(value);
            decoded.push_back(std::move(value));
        }
        values.swap(decoded);
    }
    else {
        for ( auto& value : values ) SST_SER(value);
    }
}

} // namespace detail

struct RepresentativeV1
{
    SimpleNetwork::nid_t physical_endpoint_id       = -1;
    SimpleNetwork::nid_t caller_visible_logical_nid = -1;

    constexpr bool valid() const { return physical_endpoint_id >= 0 && caller_visible_logical_nid >= 0; }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(physical_endpoint_id);
        SST_SER(caller_visible_logical_nid);
    }
};

inline constexpr bool operator==(const RepresentativeV1& lhs, const RepresentativeV1& rhs)
{
    return lhs.physical_endpoint_id == rhs.physical_endpoint_id &&
           lhs.caller_visible_logical_nid == rhs.caller_visible_logical_nid;
}

inline constexpr bool operator!=(const RepresentativeV1& lhs, const RepresentativeV1& rhs) { return !(lhs == rhs); }

struct CollectiveFabricRuntimeV1
{
    uint16_t wire_format_version = COLLECTIVE_WIRE_FORMAT_V1;
    uint32_t endpoint_reduce_vn  = 0;
    uint32_t endpoint_result_vn  = 0;
    uint32_t fabric_reduce_vn    = 0;
    uint32_t fabric_result_vn    = 0;
    uint64_t service_header_bytes = 0;
    uint64_t fabric_framing_bytes = 0;
    uint64_t accepted_flit_bits    = 0;
    uint64_t maximum_request_bits  = 0;

    bool valid(uint64_t maximum_logical_chunk_bytes) const;

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(wire_format_version);
        SST_SER(endpoint_reduce_vn);
        SST_SER(endpoint_result_vn);
        SST_SER(fabric_reduce_vn);
        SST_SER(fabric_result_vn);
        SST_SER(service_header_bytes);
        SST_SER(fabric_framing_bytes);
        SST_SER(accepted_flit_bits);
        SST_SER(maximum_request_bits);
    }
};

/**
 * Immutable, constant-size timed-execution contract.  The later canonical
 * production accepted-header record binds and projects to this type; it does
 * not add control-plane digests or global membership/tree data here.
 */
struct CollectiveRouteRuntimeV1
{
    uint16_t              schema_version = COLLECTIVE_RUNTIME_SCHEMA_V1;
    RouteIdV1             route;
    CollectiveRouteKind   route_kind = static_cast<CollectiveRouteKind>(0);
    CollectiveDataMode    data_mode  = static_cast<CollectiveDataMode>(0);
    uint64_t               operation_mask = 0;
    uint64_t               datatype_mask  = 0;
    uint16_t               descriptor_schema_version = COLLECTIVE_SERVICE_SCHEMA_V1;
    uint16_t               arithmetic_policy_version = COLLECTIVE_ARITHMETIC_POLICY_V1;
    uint32_t               accepted_invocation_quota = 0;
    uint32_t               submission_window          = 0;
    uint64_t               maximum_logical_chunk_bytes = 0;
    std::array<uint64_t, 6> maximum_chunk_elements {};
    std::optional<CollectiveFabricRuntimeV1> fabric;

    CollectiveRouteValidation validate() const;
    bool valid() const { return validate() == CollectiveRouteValidation::Valid; }

    constexpr bool supports(CollectiveOperation operation) const
    {
        return isValid(operation) && (operation_mask & operationMask(operation)) != 0;
    }

    constexpr bool supports(CollectiveDatatype datatype) const
    {
        return isValid(datatype) && (datatype_mask & datatypeMask(datatype)) != 0;
    }

    uint64_t chunkElements(CollectiveDatatype datatype) const
    {
        if ( !isValid(datatype) ) return 0;
        return maximum_chunk_elements[static_cast<uint8_t>(datatype) - 1];
    }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        uint8_t kind_code  = static_cast<uint8_t>(route_kind);
        uint8_t mode_code  = static_cast<uint8_t>(data_mode);
        uint8_t has_fabric = fabric ? 1 : 0;

        SST_SER(schema_version);
        SST_SER(route);
        SST_SER(kind_code);
        SST_SER(mode_code);
        SST_SER(operation_mask);
        SST_SER(datatype_mask);
        SST_SER(descriptor_schema_version);
        SST_SER(arithmetic_policy_version);
        SST_SER(accepted_invocation_quota);
        SST_SER(submission_window);
        SST_SER(maximum_logical_chunk_bytes);
        for ( auto& count : maximum_chunk_elements ) SST_SER(count);
        SST_SER(has_fabric);

        if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
            route_kind = static_cast<CollectiveRouteKind>(kind_code);
            data_mode  = static_cast<CollectiveDataMode>(mode_code);
            if ( !isValid(route_kind) || !isValid(data_mode) || has_fabric > 1 ) {
                throw std::invalid_argument("Invalid collective runtime enum or optional tag");
            }
            if ( has_fabric ) fabric.emplace();
            else fabric.reset();
        }
        if ( has_fabric ) SST_SER(*fabric);
    }
};

CollectivePacketValidation validateCollectivePacket(const CollectiveServiceData& data,
    const CollectiveRouteRuntimeV1& runtime, CollectiveIngressRole ingress_role, uint64_t request_size_bits);

struct EndpointParticipantProjectionV1
{
    uint64_t                     canonical_member_ordinal = 0;
    uint64_t                     logical_participant_id   = 0;
    uint32_t                     local_slot               = 0;
    ParticipantBindingIdentityV1 binding;

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(canonical_member_ordinal);
        SST_SER(logical_participant_id);
        SST_SER(local_slot);
        SST_SER(binding);
    }
};

struct EndpointFabricProjectionV1
{
    RepresentativeV1       root_representative;
    SimpleNetwork::nid_t    injection_dest_nid = -1;

    bool valid() const
    {
        return root_representative.valid() && injection_dest_nid >= 0 &&
               injection_dest_nid == root_representative.caller_visible_logical_nid;
    }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(root_representative);
        SST_SER(injection_dest_nid);
    }
};

struct EndpointRouteProjectionV1
{
    uint16_t                              schema_version = COLLECTIVE_PROJECTION_SCHEMA_V1;
    RouteIdV1                             route;
    uint64_t                              owner_component_id = 0;
    SimpleNetwork::nid_t                  physical_endpoint_id = -1;
    uint32_t                              local_participant_count = 0;
    std::vector<EndpointParticipantProjectionV1> local_participants;
    std::optional<EndpointFabricProjectionV1> fabric;

    CollectiveRouteValidation validate(const CollectiveRouteRuntimeV1& runtime) const;
    bool valid(const CollectiveRouteRuntimeV1& runtime) const
    {
        return validate(runtime) == CollectiveRouteValidation::Valid;
    }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        uint8_t has_fabric = fabric ? 1 : 0;
        SST_SER(schema_version);
        SST_SER(route);
        SST_SER(owner_component_id);
        SST_SER(physical_endpoint_id);
        SST_SER(local_participant_count);
        detail::serializeRouteVector(ser, local_participants);
        SST_SER(has_fabric);
        if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
            if ( has_fabric > 1 ) throw std::invalid_argument("Invalid endpoint projection optional tag");
            if ( has_fabric ) fabric.emplace();
            else fabric.reset();
        }
        if ( has_fabric ) SST_SER(*fabric);
    }
};

struct RouterBranchProjectionV1
{
    uint32_t         port = 0;
    RepresentativeV1 representative;

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(port);
        SST_SER(representative);
    }
};

struct RouterRouteProjectionV1
{
    uint16_t                           schema_version = COLLECTIVE_PROJECTION_SCHEMA_V1;
    RouteIdV1                          route;
    uint64_t                           owner_component_id = 0;
    bool                               root = false;
    std::optional<uint32_t>            parent_port;
    RepresentativeV1                   subtree_representative;
    RepresentativeV1                   root_representative;
    std::vector<RouterBranchProjectionV1> child_branches;
    std::vector<RouterBranchProjectionV1> local_endpoint_branches;

    CollectiveRouteValidation validate(const CollectiveRouteRuntimeV1& runtime) const;
    bool valid(const CollectiveRouteRuntimeV1& runtime) const
    {
        return validate(runtime) == CollectiveRouteValidation::Valid;
    }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        uint8_t root_code  = root ? 1 : 0;
        uint8_t has_parent = parent_port ? 1 : 0;
        SST_SER(schema_version);
        SST_SER(route);
        SST_SER(owner_component_id);
        SST_SER(root_code);
        SST_SER(has_parent);
        if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
            if ( root_code > 1 || has_parent > 1 ) {
                throw std::invalid_argument("Invalid router projection boolean or optional tag");
            }
            root = root_code != 0;
            if ( has_parent ) parent_port.emplace();
            else parent_port.reset();
        }
        if ( has_parent ) SST_SER(*parent_port);
        SST_SER(subtree_representative);
        SST_SER(root_representative);
        detail::serializeRouteVector(ser, child_branches);
        detail::serializeRouteVector(ser, local_endpoint_branches);
    }
};

/** Static and production authorities install exactly one endpoint-local view through this seam. */
class CollectiveEndpointRouteInstallTarget
{
public:
    virtual ~CollectiveEndpointRouteInstallTarget() = default;
    virtual CollectiveRouteInstallResult installCollectiveRoute(
        const CollectiveRouteRuntimeV1& runtime, EndpointRouteProjectionV1&& local_projection) = 0;
};

/** Static and production authorities install exactly one router-local view through this seam. */
class CollectiveRouterRouteInstallTarget
{
public:
    virtual ~CollectiveRouterRouteInstallTarget() = default;
    virtual CollectiveRouteInstallResult installCollectiveRoute(
        const CollectiveRouteRuntimeV1& runtime, RouterRouteProjectionV1&& local_projection) = 0;
};

} // namespace SST::Collective

#endif // SST_ELEMENTS_COLLECTIVE_ROUTE_H
