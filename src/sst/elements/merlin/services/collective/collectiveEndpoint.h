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

namespace SST::Collective {

enum class CollectiveSubmitResult : uint8_t { Accepted = 1, Retry = 2, Unsupported = 3, Invalid = 4 };
/**
 * An accepted invocation must complete successfully. Failures after acceptance
 * are terminal; software fallback is allowed only before acceptance.
 */
enum class CollectiveCompletionStatus : uint8_t { Success = 1 };

inline constexpr bool isValid(CollectiveSubmitResult value)
{
    return value >= CollectiveSubmitResult::Accepted && value <= CollectiveSubmitResult::Invalid;
}

inline constexpr bool isValid(CollectiveCompletionStatus value)
{
    return value == CollectiveCompletionStatus::Success;
}

/**
 * One endpoint's identity on a static collective route.  The NIC installs it
 * after validating its transport; a client compares it with the identity its
 * job expects before binding.
 */
struct CollectiveParticipant
{
    RouteIdV1            route;
    SimpleNetwork::nid_t physical_endpoint_id   = -1;
    SimpleNetwork::nid_t logical_participant_id = -1;
    int                  reduce_vn              = -1;
    int                  result_vn              = -1;

    constexpr bool valid() const
    {
        return route.valid() && physical_endpoint_id >= 0 && logical_participant_id >= 0 && reduce_vn >= 0 &&
               result_vn >= 0 && reduce_vn != result_vn;
    }

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(route);
        SST_SER(physical_endpoint_id);
        SST_SER(logical_participant_id);
        SST_SER(reduce_vn);
        SST_SER(result_vn);
    }
};

inline constexpr bool operator==(const CollectiveParticipant& lhs, const CollectiveParticipant& rhs)
{
    return lhs.route == rhs.route && lhs.physical_endpoint_id == rhs.physical_endpoint_id &&
           lhs.logical_participant_id == rhs.logical_participant_id && lhs.reduce_vn == rhs.reduce_vn &&
           lhs.result_vn == rhs.result_vn;
}

inline constexpr bool operator!=(const CollectiveParticipant& lhs, const CollectiveParticipant& rhs)
{
    return !(lhs == rhs);
}

/**
 * One collective call offered to an endpoint.  On Accepted the endpoint has
 * copied the source bytes and keeps the result view until it completes the
 * invocation, so the caller must keep the result buffer alive until then.
 */
struct CollectiveSubmission
{
    uint64_t              invocation_id = 0;
    CollectiveSignatureV1 signature;
    BufferView            source;
    MutableBufferView     result;
};

class CollectiveCompletionSink
{
public:
    virtual ~CollectiveCompletionSink() = default;
    /** Called exactly once per accepted invocation. */
    virtual void complete(uint64_t invocation_id, CollectiveCompletionStatus status) = 0;
};

class CollectiveReadySink
{
public:
    virtual ~CollectiveReadySink() = default;
    virtual void ready() = 0;
};

/** Native-stack-neutral asynchronous endpoint service. */
class CollectiveEndpoint
{
public:
    virtual ~CollectiveEndpoint() = default;

    /** The installed participant, or nullptr until the NIC has validated its route. */
    virtual const CollectiveParticipant* participant() const = 0;

    /** Non-owning capability query; submission remains the authoritative decision. */
    virtual bool supportsCollective(const CollectiveSignatureV1& signature) const = 0;

    /** Registers the client's sinks once; the endpoint retains only their addresses. */
    virtual bool bind(CollectiveCompletionSink& completion, CollectiveReadySink& ready) = 0;

    virtual CollectiveSubmitResult trySubmitCollective(const CollectiveSubmission& submission) = 0;

    /** Arms one level-triggered ready notification after a Retry. */
    virtual void requestCollectiveReady(const CollectiveSignatureV1& signature) = 0;
};

} // namespace SST::Collective

#endif // SST_ELEMENTS_COLLECTIVE_ENDPOINT_H
