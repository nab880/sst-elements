// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_COLLECTIVE_ENDPOINT_H
#define SST_ELEMENTS_COLLECTIVE_ENDPOINT_H

#include "collectiveTypes.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace SST::Collective {

enum class CollectiveSubmitResult : uint8_t { Accepted = 1, Retry = 2, Unsupported = 3, Invalid = 4 };
/**
 * RecoverableError is legal only after an Accepted operation has failed
 * collectively: every participant must receive the same status, all service
 * traffic and reservations for the invocation must be quiescent, and native
 * buffer owners must be able to restart the operation in software.  A local,
 * partial, or still-in-flight failure is terminal and must not use this value.
 */
enum class CollectiveCompletionStatus : uint8_t { Success = 1, RecoverableError = 2 };
enum class CollectivePendingState : uint8_t { Ready = 1, Consumed = 2 };

inline constexpr bool isValid(CollectiveSubmitResult value)
{
    return value >= CollectiveSubmitResult::Accepted && value <= CollectiveSubmitResult::Invalid;
}

inline constexpr bool isValid(CollectiveCompletionStatus value)
{
    return value == CollectiveCompletionStatus::Success || value == CollectiveCompletionStatus::RecoverableError;
}

inline constexpr bool isValid(CollectivePendingState value)
{
    return value == CollectivePendingState::Ready || value == CollectivePendingState::Consumed;
}

/** Component-local route slot.  Slot zero is valid; generation zero is not. */
struct PhysicalRouteHandleV1
{
    uint32_t slot       = 0;
    uint32_t generation = 0;

    constexpr bool valid() const { return generation != 0; }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(slot);
        SST_SER(generation);
    }
};

inline constexpr bool operator==(const PhysicalRouteHandleV1& lhs, const PhysicalRouteHandleV1& rhs)
{
    return lhs.slot == rhs.slot && lhs.generation == rhs.generation;
}

/** Stable routing identity for the participant's pre-registered completion and ready sinks. */
struct ParticipantBindingIdentityV1
{
    uint64_t adapter_component_id = 0;
    uint32_t adapter_slot         = 0;
    uint32_t generation           = 0;

    constexpr bool valid() const { return generation != 0; }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(adapter_component_id);
        SST_SER(adapter_slot);
        SST_SER(generation);
    }
};

inline constexpr bool operator==(
    const ParticipantBindingIdentityV1& lhs, const ParticipantBindingIdentityV1& rhs)
{
    return lhs.adapter_component_id == rhs.adapter_component_id && lhs.adapter_slot == rhs.adapter_slot &&
           lhs.generation == rhs.generation;
}

struct FabricParticipantRouteV1
{
    uint32_t                   endpoint_reduce_vn = 0;
    uint32_t                   endpoint_result_vn = 0;
    SimpleNetwork::nid_t       injection_dest_nid = -1;

    constexpr bool valid() const
    {
        return endpoint_reduce_vn != endpoint_result_vn &&
               endpoint_reduce_vn <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
               endpoint_result_vn <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
               injection_dest_nid >= 0;
    }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(endpoint_reduce_vn);
        SST_SER(endpoint_result_vn);
        SST_SER(injection_dest_nid);
    }
};

/**
 * Fixed-width, allocator-free native completion identity.  Generation zero is
 * reserved for an empty or moved-from token, allowing the token to remain
 * exactly sixteen bytes without an allocator or hidden validity object.
 */
class CollectiveCompletionToken
{
public:
    constexpr CollectiveCompletionToken() = default;
    constexpr CollectiveCompletionToken(uint32_t adapter_slot, uint64_t native_request_id, uint32_t generation) :
        native_request_id_(native_request_id),
        adapter_slot_(adapter_slot),
        generation_(generation)
    {}

    CollectiveCompletionToken(const CollectiveCompletionToken&)            = delete;
    CollectiveCompletionToken& operator=(const CollectiveCompletionToken&) = delete;

    constexpr CollectiveCompletionToken(CollectiveCompletionToken&& other) noexcept :
        native_request_id_(other.native_request_id_),
        adapter_slot_(other.adapter_slot_),
        generation_(other.generation_)
    {
        other.clear();
    }

    constexpr CollectiveCompletionToken& operator=(CollectiveCompletionToken&& other) noexcept
    {
        if ( this != &other ) {
            native_request_id_ = other.native_request_id_;
            adapter_slot_      = other.adapter_slot_;
            generation_        = other.generation_;
            other.clear();
        }
        return *this;
    }

    constexpr bool     valid() const { return generation_ != 0; }
    constexpr uint32_t adapterSlot() const { return adapter_slot_; }
    constexpr uint64_t nativeRequestId() const { return native_request_id_; }
    constexpr uint32_t generation() const { return generation_; }

private:
    constexpr void clear() noexcept
    {
        native_request_id_ = 0;
        adapter_slot_      = 0;
        generation_        = 0;
    }

    uint64_t native_request_id_ = 0;
    uint32_t adapter_slot_      = 0;
    uint32_t generation_        = 0;
};

static_assert(sizeof(CollectiveCompletionToken) == 16, "Collective completion tokens must remain sixteen bytes");
static_assert(!std::is_copy_constructible_v<CollectiveCompletionToken>);
static_assert(!std::is_copy_assignable_v<CollectiveCompletionToken>);
static_assert(std::is_nothrow_move_constructible_v<CollectiveCompletionToken>);
static_assert(std::is_nothrow_move_assignable_v<CollectiveCompletionToken>);

/** O(1), component-local binding published only after route acceptance. */
struct AcceptedParticipantHandle
{
    uint16_t                         schema_version = COLLECTIVE_RUNTIME_SCHEMA_V1;
    RouteIdV1                        route;
    PhysicalRouteHandleV1            physical_route;
    CollectiveRouteKind              route_kind = static_cast<CollectiveRouteKind>(0);
    CollectiveDataMode               data_mode  = static_cast<CollectiveDataMode>(0);
    SimpleNetwork::nid_t              physical_endpoint_id = -1;
    uint32_t                          local_participant_slot  = 0;
    uint32_t                          local_participant_count = 0;
    uint64_t                          logical_participant_id  = 0;
    ParticipantBindingIdentityV1      binding;
    uint32_t                          accepted_invocation_quota = 0;
    uint32_t                          submission_window          = 0;
    std::optional<FabricParticipantRouteV1> fabric;

    bool valid() const
    {
        if ( schema_version != COLLECTIVE_RUNTIME_SCHEMA_V1 || !route.valid() || !physical_route.valid() ||
             !isValid(route_kind) || !isValid(data_mode) || physical_endpoint_id < 0 ||
             local_participant_count == 0 || local_participant_slot >= local_participant_count || !binding.valid() ||
             accepted_invocation_quota == 0 || submission_window == 0 ) {
            return false;
        }
        return route_kind == CollectiveRouteKind::FabricTree ? (fabric && fabric->valid()) : !fabric.has_value();
    }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        uint8_t kind_code = static_cast<uint8_t>(route_kind);
        uint8_t mode_code = static_cast<uint8_t>(data_mode);
        uint8_t has_fabric = fabric ? 1 : 0;

        SST_SER(schema_version);
        SST_SER(route);
        SST_SER(physical_route);
        SST_SER(kind_code);
        SST_SER(mode_code);
        SST_SER(physical_endpoint_id);
        SST_SER(local_participant_slot);
        SST_SER(local_participant_count);
        SST_SER(logical_participant_id);
        SST_SER(binding);
        SST_SER(accepted_invocation_quota);
        SST_SER(submission_window);
        SST_SER(has_fabric);

        if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
            route_kind = static_cast<CollectiveRouteKind>(kind_code);
            data_mode  = static_cast<CollectiveDataMode>(mode_code);
            if ( !isValid(route_kind) || !isValid(data_mode) || has_fabric > 1 ) {
                throw std::invalid_argument("Invalid accepted participant handle enum or optional tag");
            }
            if ( has_fabric ) fabric.emplace();
            else fabric.reset();
        }
        if ( has_fabric ) SST_SER(*fabric);
    }
};

struct CollectivePending
{
    AcceptedParticipantHandle participant;
    uint64_t                  invocation_id = 0;
    CollectiveSignatureV1     signature;
    BufferView                source;
    MutableBufferView         result;
    CollectiveCompletionToken completion;
    CollectivePendingState    state = CollectivePendingState::Ready;

    CollectivePending() = default;
    CollectivePending(const CollectivePending&)            = delete;
    CollectivePending& operator=(const CollectivePending&) = delete;
    CollectivePending(CollectivePending&&) noexcept         = default;
    CollectivePending& operator=(CollectivePending&&) noexcept = default;

    bool readyForSubmit() const
    {
        return state == CollectivePendingState::Ready && completion.valid();
    }

    /** Call only after all validation and bounded reservations commit. */
    CollectiveCompletionToken consumeAfterAcceptance()
    {
        if ( !readyForSubmit() ) {
            throw std::logic_error("CollectivePending is not ready for accepted ownership transfer");
        }
        CollectiveCompletionToken token(std::move(completion));
        state = CollectivePendingState::Consumed;
        return token;
    }
};

class CollectiveCompletionSink
{
public:
    virtual ~CollectiveCompletionSink() = default;
    /** Returns ownership; RecoverableError additionally promises the collective-wide restart contract above. */
    virtual void complete(CollectiveCompletionToken&& token, CollectiveCompletionStatus status) = 0;
};

class CollectiveReadySink
{
public:
    virtual ~CollectiveReadySink() = default;
    virtual void ready(const AcceptedParticipantHandle& participant) = 0;
};

/** Native-stack-neutral asynchronous endpoint service. */
class CollectiveEndpoint
{
public:
    virtual ~CollectiveEndpoint() = default;

    /** Non-owning capability query; submission remains the authoritative decision. */
    virtual bool supportsCollective(const CollectiveSignatureV1& signature) const = 0;

    /** Registration is setup-time and non-owning; implementations retain only the interface addresses. */
    virtual bool bindParticipant(const AcceptedParticipantHandle& participant, CollectiveCompletionSink& completion,
        CollectiveReadySink& ready) = 0;

    /** Only Accepted may call pending.consumeAfterAcceptance(). */
    virtual CollectiveSubmitResult trySubmitCollective(CollectivePending& pending) = 0;

    /** Arms the participant's pre-registered level-triggered ready notification. */
    virtual void requestCollectiveReady(const AcceptedParticipantHandle& participant,
        const CollectiveSignatureV1& signature) = 0;
};

} // namespace SST::Collective

#endif // SST_ELEMENTS_COLLECTIVE_ENDPOINT_H
