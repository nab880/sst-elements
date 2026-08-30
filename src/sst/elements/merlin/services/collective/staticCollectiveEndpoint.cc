// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include <sst_config.h>

#include "staticCollectiveEndpoint.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace SST::Collective {

namespace {

bool sameFabric(const AcceptedParticipantHandle& lhs, const AcceptedParticipantHandle& rhs)
{
    return lhs.fabric.has_value() == rhs.fabric.has_value() &&
        (!lhs.fabric ||
            (lhs.fabric->endpoint_reduce_vn == rhs.fabric->endpoint_reduce_vn &&
                lhs.fabric->endpoint_result_vn == rhs.fabric->endpoint_result_vn &&
                lhs.fabric->injection_dest_nid == rhs.fabric->injection_dest_nid));
}

} // namespace

const AcceptedParticipantHandle* StaticCollectiveEndpointBase::participant(uint32_t local_slot) const
{
    return accepted_.valid() && local_slot == 0 ? &accepted_ : nullptr;
}

bool StaticCollectiveEndpointBase::supportsCollective(const CollectiveSignatureV1& signature) const
{
    return signature.valid() && signature == STATIC_COLLECTIVE_SIGNATURE_V1;
}

bool StaticCollectiveEndpointBase::installParticipant(const AcceptedParticipantHandle& participant)
{
    if ( accepted_.valid() || !participant.valid() || participant.local_participant_slot != 0 ||
         participant.local_participant_count != 1 ||
         participant.route_kind != CollectiveRouteKind::FabricTree ||
         participant.data_mode != CollectiveDataMode::Functional || !participant.fabric ||
         participant.fabric->endpoint_reduce_vn != 0 || participant.fabric->endpoint_result_vn != 1 ||
         participant.accepted_invocation_quota != 1 || participant.submission_window != 1 ) {
        return false;
    }

    accepted_ = participant;
    return true;
}

bool StaticCollectiveEndpointBase::sameParticipant(const AcceptedParticipantHandle& participant) const
{
    return accepted_.valid() && participant.schema_version == accepted_.schema_version &&
        participant.route == accepted_.route && participant.physical_route == accepted_.physical_route &&
        participant.route_kind == accepted_.route_kind && participant.data_mode == accepted_.data_mode &&
        participant.physical_endpoint_id == accepted_.physical_endpoint_id &&
        participant.local_participant_slot == accepted_.local_participant_slot &&
        participant.local_participant_count == accepted_.local_participant_count &&
        participant.logical_participant_id == accepted_.logical_participant_id &&
        participant.binding == accepted_.binding &&
        participant.accepted_invocation_quota == accepted_.accepted_invocation_quota &&
        participant.submission_window == accepted_.submission_window && sameFabric(participant, accepted_);
}

bool StaticCollectiveEndpointBase::bindParticipant(const AcceptedParticipantHandle& participant,
    CollectiveCompletionSink& completion, CollectiveReadySink& ready)
{
    if ( completion_ != nullptr || &participant != &accepted_ || !sameParticipant(participant) ) return false;
    completion_ = &completion;
    ready_sink_ = &ready;
    return true;
}

CollectiveSubmitResult StaticCollectiveEndpointBase::trySubmitCollective(CollectivePending& pending)
{
    if ( !accepted_.valid() || completion_ == nullptr || ready_sink_ == nullptr || !pending.readyForSubmit() ||
         !pending.participant.valid() || !sameParticipant(pending.participant) ) {
        return CollectiveSubmitResult::Invalid;
    }
    if ( !pending.signature.valid() ) {
        return CollectiveSubmitResult::Invalid;
    }
    if ( !supportsCollective(pending.signature) ) {
        return CollectiveSubmitResult::Unsupported;
    }
    const auto payload_bytes = pending.signature.payloadBytes();
    if ( pending.invocation_id == 0 || pending.invocation_id <= retired_invocation_ ||
         !payload_bytes || *payload_bytes != CollectiveServiceData::VALUE_BYTES ||
         pending.source.data == nullptr || pending.source.bytes != *payload_bytes ||
         pending.result.data == nullptr || pending.result.bytes != *payload_bytes ) {
        return CollectiveSubmitResult::Invalid;
    }
    if ( active_invocation_ != 0 || !transportReady(pending.signature) ) {
        return CollectiveSubmitResult::Retry;
    }

    StaticCollectiveContribution contribution;
    contribution.route = accepted_.route;
    contribution.invocation_id = pending.invocation_id;
    contribution.signature = pending.signature;
    contribution.value.resize(*payload_bytes);
    std::memcpy(contribution.value.data(), pending.source.data, contribution.value.size());
    const uint64_t          invocation_id = contribution.invocation_id;
    const CollectiveSignatureV1 signature = contribution.signature;
    const MutableBufferView result        = pending.result;
    CollectiveCompletionToken token(pending.consumeAfterAcceptance());
    result_            = result;
    active_signature_  = signature;
    token_             = std::move(token);
    active_invocation_ = invocation_id;
    ready_armed_       = false;
    commitContribution(accepted_, std::move(contribution));
    return CollectiveSubmitResult::Accepted;
}

void StaticCollectiveEndpointBase::requestCollectiveReady(
    const AcceptedParticipantHandle& participant, const CollectiveSignatureV1& signature)
{
    if ( ready_sink_ == nullptr || !sameParticipant(participant) || !supportsCollective(signature) ) return;
    ready_armed_ = true;
    ready_signature_ = signature;
    notifyReadyIfPossible();
}

bool StaticCollectiveEndpointBase::notifyReadyIfPossible()
{
    if ( !ready_armed_ || active_invocation_ != 0 || ready_sink_ == nullptr ||
         !transportReady(ready_signature_) ) {
        return false;
    }
    ready_armed_ = false;
    ready_signature_ = {};
    ready_sink_->ready(accepted_);
    return true;
}

bool StaticCollectiveEndpointBase::completeSuccess(const StaticCollectiveResult& result)
{
    if ( active_invocation_ == 0 || !result.valid() || result.route != accepted_.route ||
         result.invocation_id != active_invocation_ || result.signature != active_signature_ ||
         !token_.valid() || completion_ == nullptr || result_.data == nullptr ||
         result_.bytes != result.value.size() ) {
        return false;
    }

    std::memcpy(result_.data, result.value.data(), result.value.size());
    CollectiveCompletionToken token(std::move(token_));
    result_               = {};
    active_signature_     = {};
    retired_invocation_   = active_invocation_;
    active_invocation_    = 0;
    CollectiveCompletionSink* const completion = completion_;
    completion->complete(std::move(token), CollectiveCompletionStatus::Success);
    notifyReadyIfPossible();
    return true;
}

bool StaticCollectiveEndpointBase::quiescent() const
{
    return active_invocation_ == 0 && !active_signature_.valid() && !token_.valid();
}

bool supportsStaticCollectiveTransport(const SimpleNetwork& network, int reduce_vn, int result_vn)
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
        capability.max_atomic_request_bits_by_vn[reduce_vn] >= CollectiveServiceData::MODELED_REQUEST_BITS &&
        capability.max_atomic_request_bits_by_vn[result_vn] >= CollectiveServiceData::MODELED_REQUEST_BITS;
}

} // namespace SST::Collective
