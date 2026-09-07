// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_STATIC_COLLECTIVE_ENDPOINT_H
#define SST_ELEMENTS_STATIC_COLLECTIVE_ENDPOINT_H

#include "collectiveEndpoint.h"
#include "collectiveServiceData.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

namespace SST::Collective {

/**
 * Shared state for the static collective profile: one participant per
 * endpoint, one active invocation, and exactly STATIC_COLLECTIVE_SIGNATURE_V1.
 * The NIC-specific subclass owns transport readiness and packet injection.
 */
class StaticCollectiveEndpointBase : public CollectiveEndpoint
{
public:
    const CollectiveParticipant* participant() const final { return participant_.valid() ? &participant_ : nullptr; }

    bool supportsCollective(const CollectiveSignatureV1& signature) const final
    {
        return signature.valid() && signature == STATIC_COLLECTIVE_SIGNATURE_V1;
    }

    bool bind(CollectiveCompletionSink& completion, CollectiveReadySink& ready) final
    {
        if ( completion_ != nullptr || !participant_.valid() ) return false;
        completion_ = &completion;
        ready_sink_ = &ready;
        return true;
    }

    CollectiveSubmitResult trySubmitCollective(const CollectiveSubmission& submission) final
    {
        if ( !participant_.valid() || completion_ == nullptr || ready_sink_ == nullptr ||
             !submission.signature.valid() ) {
            return CollectiveSubmitResult::Invalid;
        }
        if ( !supportsCollective(submission.signature) ) return CollectiveSubmitResult::Unsupported;

        const std::optional<uint64_t> payload_bytes = submission.signature.payloadBytes();
        if ( submission.invocation_id == 0 || submission.invocation_id <= retired_invocation_ || !payload_bytes ||
             submission.source.data == nullptr || submission.source.bytes != *payload_bytes ||
             submission.result.data == nullptr || submission.result.bytes != *payload_bytes ) {
            return CollectiveSubmitResult::Invalid;
        }
        if ( active_invocation_ != 0 || !transportReady(submission.signature) ) {
            return CollectiveSubmitResult::Retry;
        }

        StaticCollectiveContribution contribution;
        contribution.route         = participant_.route;
        contribution.invocation_id = submission.invocation_id;
        contribution.signature     = submission.signature;
        contribution.value.resize(*payload_bytes);
        std::memcpy(contribution.value.data(), submission.source.data, contribution.value.size());

        result_            = submission.result;
        active_invocation_ = submission.invocation_id;
        ready_armed_       = false;
        commitContribution(std::move(contribution));
        return CollectiveSubmitResult::Accepted;
    }

    void requestCollectiveReady(const CollectiveSignatureV1& signature) final
    {
        if ( ready_sink_ == nullptr || !supportsCollective(signature) ) return;
        ready_armed_ = true;
        notifyReadyIfPossible();
    }

    bool notifyReadyIfPossible()
    {
        if ( !ready_armed_ || active_invocation_ != 0 || ready_sink_ == nullptr ||
             !transportReady(STATIC_COLLECTIVE_SIGNATURE_V1) ) {
            return false;
        }
        ready_armed_ = false;
        ready_sink_->ready();
        return true;
    }

    bool quiescent() const { return active_invocation_ == 0; }

protected:
    bool installParticipant(const CollectiveParticipant& participant)
    {
        if ( participant_.valid() || !participant.valid() ) return false;
        participant_ = participant;
        return true;
    }

    const CollectiveParticipant& installedParticipant() const { return participant_; }
    uint64_t                     activeInvocation() const { return active_invocation_; }

    bool completeSuccess(const StaticCollectiveResult& result)
    {
        if ( active_invocation_ == 0 || !result.valid() || result.route != participant_.route ||
             result.invocation_id != active_invocation_ || result.signature != STATIC_COLLECTIVE_SIGNATURE_V1 ||
             completion_ == nullptr || result_.data == nullptr || result_.bytes != result.value.size() ) {
            return false;
        }

        std::memcpy(result_.data, result.value.data(), result.value.size());
        const uint64_t invocation_id = active_invocation_;
        result_             = {};
        retired_invocation_ = invocation_id;
        active_invocation_  = 0;
        completion_->complete(invocation_id, CollectiveCompletionStatus::Success);
        notifyReadyIfPossible();
        return true;
    }

    virtual bool transportReady(const CollectiveSignatureV1& signature) const = 0;
    /**
     * Commits an accepted contribution or terminates.  The hook cannot
     * decline or roll back, and transport completion must arrive after
     * trySubmitCollective() returns to its caller.
     */
    virtual void commitContribution(StaticCollectiveContribution&& contribution) noexcept = 0;

private:
    CollectiveParticipant     participant_;
    CollectiveCompletionSink* completion_ = nullptr;
    CollectiveReadySink*      ready_sink_ = nullptr;
    MutableBufferView         result_;
    uint64_t                  active_invocation_  = 0;
    uint64_t                  retired_invocation_ = 0;
    bool                      ready_armed_        = false;
};

/** Checks the fixed sidecar, schema, feature, and atomic-packet transport contract. */
inline bool supportsStaticCollectiveTransport(const SimpleNetwork& network, int reduce_vn, int result_vn)
{
    if ( reduce_vn < 0 || result_vn < 0 || reduce_vn == result_vn ) return false;

    SimpleNetwork::NetworkServiceCapability capability;
    constexpr SimpleNetwork::NetworkServiceFeatureMask required =
        SimpleNetwork::SERVICE_FEATURE_SIDECAR_PRESERVATION |
        SimpleNetwork::SERVICE_FEATURE_TRANSACTIONAL_TIMED_SEND |
        SimpleNetwork::SERVICE_FEATURE_SERIALIZATION |
        SimpleNetwork::SERVICE_FEATURE_INTERMEDIATE_TERMINATION_SAFE |
        SimpleNetwork::SERVICE_FEATURE_FRESH_BASE_REQUEST_TAG_FIRST_RECEIVE;
    const auto maximum_vn = static_cast<size_t>(std::max(reduce_vn, result_vn));
    return network.queryServiceCapability(COLLECTIVE_SERVICE_ID, capability) &&
           capability.isValidFor(COLLECTIVE_SERVICE_ID) && (capability.features & required) == required &&
           capability.min_schema_version <= COLLECTIVE_SERVICE_SCHEMA_V1 &&
           capability.max_schema_version >= COLLECTIVE_SERVICE_SCHEMA_V1 &&
           capability.request_data_token == CollectiveServiceData::DATA_TOKEN &&
           capability.min_request_schema_version <= CollectiveServiceData::MIN_SCHEMA_VERSION &&
           capability.max_request_schema_version >= CollectiveServiceData::MAX_SCHEMA_VERSION &&
           capability.max_atomic_request_bits_by_vn.size() > maximum_vn &&
           capability.max_atomic_request_bits_by_vn[reduce_vn] >= STATIC_COLLECTIVE_REQUEST_BITS &&
           capability.max_atomic_request_bits_by_vn[result_vn] >= STATIC_COLLECTIVE_REQUEST_BITS;
}

} // namespace SST::Collective

#endif // SST_ELEMENTS_STATIC_COLLECTIVE_ENDPOINT_H
