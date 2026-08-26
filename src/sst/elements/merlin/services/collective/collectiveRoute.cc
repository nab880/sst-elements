// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "collectiveRoute.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace SST::Collective {

namespace {

constexpr uint64_t VALID_OPERATION_MASK = operationMask(CollectiveOperation::Sum) |
                                          operationMask(CollectiveOperation::Min) |
                                          operationMask(CollectiveOperation::Max);
constexpr uint64_t VALID_DATATYPE_MASK = datatypeMask(CollectiveDatatype::I32) |
                                         datatypeMask(CollectiveDatatype::U32) |
                                         datatypeMask(CollectiveDatatype::I64) |
                                         datatypeMask(CollectiveDatatype::U64) |
                                         datatypeMask(CollectiveDatatype::F32) |
                                         datatypeMask(CollectiveDatatype::F64);

bool checkedAdd(uint64_t lhs, uint64_t rhs, uint64_t& result)
{
    if ( lhs > std::numeric_limits<uint64_t>::max() - rhs ) return false;
    result = lhs + rhs;
    return true;
}

bool validPort(uint32_t port)
{
    return port <= static_cast<uint32_t>(std::numeric_limits<int>::max());
}

struct BindingHash
{
    size_t operator()(const ParticipantBindingIdentityV1& binding) const noexcept
    {
        uint64_t value = binding.adapter_component_id;
        value ^= uint64_t { binding.adapter_slot } << 32;
        value ^= binding.generation;
        value ^= value >> 33;
        value *= UINT64_C(0xff51afd7ed558ccd);
        value ^= value >> 33;
        return static_cast<size_t>(value);
    }
};

} // namespace

bool
CollectiveFabricRuntimeV1::valid(uint64_t maximum_logical_chunk_bytes) const
{
    if ( wire_format_version != COLLECTIVE_WIRE_FORMAT_V1 || maximum_logical_chunk_bytes == 0 ||
         endpoint_reduce_vn == endpoint_result_vn || fabric_reduce_vn == fabric_result_vn ||
         endpoint_reduce_vn > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
         endpoint_result_vn > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
         fabric_reduce_vn > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
         fabric_result_vn > static_cast<uint32_t>(std::numeric_limits<int>::max()) || accepted_flit_bits == 0 ||
         maximum_request_bits == 0 ) {
        return false;
    }

    uint64_t modeled_bytes;
    if ( !checkedAdd(service_header_bytes, maximum_logical_chunk_bytes, modeled_bytes) ||
         !checkedAdd(modeled_bytes, fabric_framing_bytes, modeled_bytes) ||
         modeled_bytes > std::numeric_limits<uint64_t>::max() / 8 ) {
        return false;
    }
    return modeled_bytes * 8 <= maximum_request_bits;
}

CollectiveRouteValidation
CollectiveRouteRuntimeV1::validate() const
{
    if ( schema_version != COLLECTIVE_RUNTIME_SCHEMA_V1 ) {
        return CollectiveRouteValidation::InvalidRuntimeSchema;
    }
    if ( !route.valid() ) return CollectiveRouteValidation::InvalidRoute;
    if ( !isValid(route_kind) ) return CollectiveRouteValidation::InvalidRouteKind;
    if ( !isValid(data_mode) ) return CollectiveRouteValidation::InvalidDataMode;
    if ( operation_mask == 0 || (operation_mask & ~VALID_OPERATION_MASK) != 0 ) {
        return CollectiveRouteValidation::InvalidOperationMask;
    }
    if ( datatype_mask == 0 || (datatype_mask & ~VALID_DATATYPE_MASK) != 0 ) {
        return CollectiveRouteValidation::InvalidDatatypeMask;
    }
    if ( descriptor_schema_version != COLLECTIVE_SERVICE_SCHEMA_V1 ) {
        return CollectiveRouteValidation::InvalidDescriptorSchema;
    }
    if ( arithmetic_policy_version != COLLECTIVE_ARITHMETIC_POLICY_V1 ) {
        return CollectiveRouteValidation::InvalidArithmeticPolicy;
    }
    if ( accepted_invocation_quota == 0 ) return CollectiveRouteValidation::InvalidQuota;
    if ( submission_window == 0 ) return CollectiveRouteValidation::InvalidWindow;
    if ( maximum_logical_chunk_bytes == 0 ) return CollectiveRouteValidation::InvalidChunkLimit;

    for ( uint8_t code = static_cast<uint8_t>(CollectiveDatatype::I32);
          code <= static_cast<uint8_t>(CollectiveDatatype::F64); ++code ) {
        const auto datatype = static_cast<CollectiveDatatype>(code);
        const auto index    = static_cast<size_t>(code - 1);
        const bool supported = supports(datatype);
        const uint64_t expected = maximum_logical_chunk_bytes / datatypeSize(datatype);
        if ( supported ) {
            if ( expected == 0 || maximum_chunk_elements[index] != expected ) {
                return CollectiveRouteValidation::InvalidChunkLimit;
            }
        }
        else if ( maximum_chunk_elements[index] != 0 ) {
            return CollectiveRouteValidation::InvalidChunkLimit;
        }
    }

    if ( route_kind == CollectiveRouteKind::FabricTree ) {
        if ( !fabric || !fabric->valid(maximum_logical_chunk_bytes) ) {
            return CollectiveRouteValidation::InvalidFabricConfiguration;
        }
    }
    else if ( fabric ) {
        return CollectiveRouteValidation::InvalidFabricConfiguration;
    }

    return CollectiveRouteValidation::Valid;
}

CollectivePacketValidation
validateCollectivePacket(const CollectiveServiceData& data, const CollectiveRouteRuntimeV1& runtime,
    CollectiveIngressRole ingress_role, uint64_t request_size_bits)
{
    if ( data.validateIntrinsic() != DescriptorValidation::Valid ) {
        return CollectivePacketValidation::MalformedDescriptor;
    }
    if ( !runtime.valid() ) return CollectivePacketValidation::InvalidRuntime;
    if ( runtime.route_kind != CollectiveRouteKind::FabricTree || !runtime.fabric ) {
        return CollectivePacketValidation::EndpointLocalHasNoPacket;
    }
    if ( data.fields.route != runtime.route ) return CollectivePacketValidation::RouteMismatch;
    if ( !runtime.supports(data.fields.operation) ) {
        return CollectivePacketValidation::UnsupportedOperation;
    }
    if ( !runtime.supports(data.fields.datatype) ) {
        return CollectivePacketValidation::UnsupportedDatatype;
    }
    if ( data.fields.service_schema_version != runtime.descriptor_schema_version ||
         data.fields.wire_format_version != runtime.fabric->wire_format_version ) {
        return CollectivePacketValidation::SchemaMismatch;
    }
    if ( data.fields.arithmetic_policy != runtime.arithmetic_policy_version ) {
        return CollectivePacketValidation::ArithmeticPolicyMismatch;
    }

    const bool contribution_ingress = ingress_role == CollectiveIngressRole::LocalEndpointContribution ||
                                      ingress_role == CollectiveIngressRole::ChildContribution;
    const bool result_ingress = ingress_role == CollectiveIngressRole::ParentResult;
    if ( (!contribution_ingress && !result_ingress) ||
         (contribution_ingress && data.fields.direction != CollectiveDirection::Contribution) ||
         (result_ingress && data.fields.direction != CollectiveDirection::Result) ) {
        return CollectivePacketValidation::DirectionMismatch;
    }

    const uint8_t expected_data_present = runtime.data_mode == CollectiveDataMode::Functional ? 1 : 0;
    if ( data.fields.data_present != expected_data_present ) {
        return CollectivePacketValidation::DataModeMismatch;
    }

    const uint64_t chunk_elements = runtime.chunkElements(data.fields.datatype);
    if ( chunk_elements == 0 ) return CollectivePacketValidation::InvalidChunkLayout;
    const uint64_t expected_chunks = data.fields.total_elements / chunk_elements +
                                     (data.fields.total_elements % chunk_elements != 0 ? 1 : 0);
    if ( expected_chunks == 0 || expected_chunks > UINT32_MAX || data.fields.total_chunks != expected_chunks ||
         data.fields.chunk_index > std::numeric_limits<uint64_t>::max() / chunk_elements ) {
        return CollectivePacketValidation::InvalidChunkLayout;
    }
    const uint64_t expected_offset = uint64_t { data.fields.chunk_index } * chunk_elements;
    if ( expected_offset >= data.fields.total_elements ) {
        return CollectivePacketValidation::InvalidChunkLayout;
    }
    const uint64_t expected_count = data.fields.chunk_index + 1 == data.fields.total_chunks ?
                                        data.fields.total_elements - expected_offset :
                                        chunk_elements;
    if ( data.fields.element_offset != expected_offset || data.fields.element_count != expected_count ) {
        return CollectivePacketValidation::InvalidChunkLayout;
    }

    uint64_t modeled_bytes;
    if ( !checkedAdd(runtime.fabric->service_header_bytes, data.fields.logical_payload_bytes, modeled_bytes) ||
         !checkedAdd(modeled_bytes, runtime.fabric->fabric_framing_bytes, modeled_bytes) ||
         modeled_bytes > std::numeric_limits<uint64_t>::max() / 8 ) {
        return CollectivePacketValidation::ModeledSizeOverflow;
    }
    if ( data.fields.modeled_wire_bytes != modeled_bytes ) {
        return CollectivePacketValidation::ModeledSizeMismatch;
    }
    const uint64_t expected_request_bits = modeled_bytes * 8;
    if ( request_size_bits != expected_request_bits ||
         expected_request_bits > runtime.fabric->maximum_request_bits ) {
        return CollectivePacketValidation::RequestSizeMismatch;
    }
    return CollectivePacketValidation::Valid;
}

CollectiveRouteValidation
EndpointRouteProjectionV1::validate(const CollectiveRouteRuntimeV1& runtime) const
{
    if ( !runtime.valid() ) return CollectiveRouteValidation::InvalidRoute;
    if ( schema_version != COLLECTIVE_PROJECTION_SCHEMA_V1 ) {
        return CollectiveRouteValidation::InvalidProjectionSchema;
    }
    if ( route != runtime.route ) return CollectiveRouteValidation::RouteMismatch;
    if ( physical_endpoint_id < 0 ) return CollectiveRouteValidation::InvalidEndpoint;
    if ( local_participant_count == 0 || local_participants.size() != local_participant_count ) {
        return CollectiveRouteValidation::InvalidParticipantCount;
    }

    std::unordered_set<uint64_t> logical_participants;
    std::unordered_set<ParticipantBindingIdentityV1, BindingHash> bindings;
    logical_participants.reserve(local_participants.size());
    bindings.reserve(local_participants.size());

    for ( size_t i = 0; i < local_participants.size(); ++i ) {
        const auto& participant = local_participants[i];
        if ( participant.local_slot != i ) return CollectiveRouteValidation::InvalidParticipant;
        if ( !participant.binding.valid() ) return CollectiveRouteValidation::InvalidBinding;
        if ( i != 0 && participant.canonical_member_ordinal <= local_participants[i - 1].canonical_member_ordinal ) {
            return CollectiveRouteValidation::InvalidParticipant;
        }
        if ( !logical_participants.insert(participant.logical_participant_id).second ) {
            return CollectiveRouteValidation::InvalidParticipant;
        }
        if ( !bindings.insert(participant.binding).second ) return CollectiveRouteValidation::InvalidBinding;
    }

    if ( runtime.route_kind == CollectiveRouteKind::FabricTree ) {
        if ( !fabric || !fabric->valid() ) return CollectiveRouteValidation::InvalidRepresentative;
    }
    else if ( fabric ) {
        return CollectiveRouteValidation::RouteKindMismatch;
    }

    return CollectiveRouteValidation::Valid;
}

CollectiveRouteValidation
RouterRouteProjectionV1::validate(const CollectiveRouteRuntimeV1& runtime) const
{
    if ( !runtime.valid() ) return CollectiveRouteValidation::InvalidRoute;
    if ( runtime.route_kind != CollectiveRouteKind::FabricTree ) {
        return CollectiveRouteValidation::RouteKindMismatch;
    }
    if ( schema_version != COLLECTIVE_PROJECTION_SCHEMA_V1 ) {
        return CollectiveRouteValidation::InvalidProjectionSchema;
    }
    if ( route != runtime.route ) return CollectiveRouteValidation::RouteMismatch;
    if ( root == parent_port.has_value() ) return CollectiveRouteValidation::InvalidParent;
    if ( parent_port && !validPort(*parent_port) ) return CollectiveRouteValidation::InvalidPort;
    if ( !subtree_representative.valid() || !root_representative.valid() ) {
        return CollectiveRouteValidation::InvalidRepresentative;
    }
    if ( root && subtree_representative != root_representative ) {
        return CollectiveRouteValidation::InvalidRepresentative;
    }
    if ( child_branches.empty() && local_endpoint_branches.empty() ) {
        return CollectiveRouteValidation::EmptyIngress;
    }
    if ( child_branches.size() > UINT32_MAX || local_endpoint_branches.size() > UINT32_MAX ||
         child_branches.size() > UINT32_MAX - local_endpoint_branches.size() ) {
        return CollectiveRouteValidation::InvalidPort;
    }

    std::unordered_set<uint32_t> ports;
    ports.reserve(child_branches.size() + local_endpoint_branches.size() + (parent_port ? 1 : 0));
    if ( parent_port ) ports.insert(*parent_port);

    for ( const auto& branch : child_branches ) {
        if ( !validPort(branch.port) ) return CollectiveRouteValidation::InvalidPort;
        if ( !branch.representative.valid() ) return CollectiveRouteValidation::InvalidRepresentative;
        if ( !ports.insert(branch.port).second ) return CollectiveRouteValidation::DuplicatePort;
    }

    for ( const auto& branch : local_endpoint_branches ) {
        if ( !validPort(branch.port) ) return CollectiveRouteValidation::InvalidPort;
        if ( !branch.representative.valid() ) return CollectiveRouteValidation::InvalidRepresentative;
        if ( !ports.insert(branch.port).second ) return CollectiveRouteValidation::DuplicatePort;
    }

    return CollectiveRouteValidation::Valid;
}

} // namespace SST::Collective
