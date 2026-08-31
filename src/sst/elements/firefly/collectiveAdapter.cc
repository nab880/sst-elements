// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include <sst_config.h>

#include "collectiveAdapter.h"
#include "nic.h"
#include "virtNic.h"

#include <cstring>
#include <memory>

namespace SST::Firefly {
namespace {

using namespace SST::Collective;

bool sameParticipantValue(
    const AcceptedParticipantHandle& lhs, const AcceptedParticipantHandle& rhs)
{
    const bool same_fabric = lhs.fabric.has_value() == rhs.fabric.has_value() &&
        (!lhs.fabric ||
            (lhs.fabric->endpoint_reduce_vn == rhs.fabric->endpoint_reduce_vn &&
             lhs.fabric->endpoint_result_vn == rhs.fabric->endpoint_result_vn &&
             lhs.fabric->injection_dest_nid == rhs.fabric->injection_dest_nid));
    return lhs.schema_version == rhs.schema_version && lhs.route == rhs.route &&
        lhs.physical_route == rhs.physical_route && lhs.route_kind == rhs.route_kind &&
        lhs.data_mode == rhs.data_mode && lhs.physical_endpoint_id == rhs.physical_endpoint_id &&
        lhs.local_participant_slot == rhs.local_participant_slot &&
        lhs.local_participant_count == rhs.local_participant_count &&
        lhs.logical_participant_id == rhs.logical_participant_id && lhs.binding == rhs.binding &&
        lhs.accepted_invocation_quota == rhs.accepted_invocation_quota &&
        lhs.submission_window == rhs.submission_window && same_fabric;
}

} // namespace

FireflyCollectiveEndpoint::FireflyCollectiveEndpoint(VirtNic& owner) : owner_(owner) {}

bool FireflyCollectiveEndpoint::publish(const AcceptedParticipantHandle& participant)
{
    if ( published_ || !participant.valid() || participant.local_participant_slot != 0 ||
            participant.local_participant_count != 1 ||
            participant.route_kind != CollectiveRouteKind::FabricTree ||
            participant.data_mode != CollectiveDataMode::Functional || !participant.fabric ||
            participant.accepted_invocation_quota != 1 || participant.submission_window != 1 ||
            participant.fabric->endpoint_reduce_vn != 0 ||
            participant.fabric->endpoint_result_vn != 1 ) {
        return false;
    }
    accepted_ = participant;
    published_ = true;
    return true;
}

const AcceptedParticipantHandle* FireflyCollectiveEndpoint::participant(uint32_t local_slot) const
{
    return published_ && local_slot == 0 ? &accepted_ : nullptr;
}

bool FireflyCollectiveEndpoint::supportsCollective(
    const CollectiveSignatureV1& signature) const
{
    return signature.valid() && signature == STATIC_COLLECTIVE_SIGNATURE_V1;
}

bool FireflyCollectiveEndpoint::sameParticipant(const AcceptedParticipantHandle& participant) const
{
    return published_ && sameParticipantValue(participant, accepted_);
}

bool FireflyCollectiveEndpoint::bindParticipant(const AcceptedParticipantHandle& participant,
    CollectiveCompletionSink& completion, CollectiveReadySink& ready)
{
    if ( completion_ != nullptr || &participant != &accepted_ || !sameParticipant(participant) ) {
        return false;
    }
    completion_ = &completion;
    ready_ = &ready;
    return true;
}

CollectiveSubmitResult FireflyCollectiveEndpoint::trySubmitCollective(CollectivePending& pending)
{
    if ( !published_ || completion_ == nullptr || !pending.readyForSubmit() ||
            !pending.participant.valid() || !sameParticipant(pending.participant) ) {
        return CollectiveSubmitResult::Invalid;
    }
    if ( !pending.signature.valid() ) return CollectiveSubmitResult::Invalid;
    if ( !supportsCollective(pending.signature) ) return CollectiveSubmitResult::Unsupported;
    const auto payload_bytes = pending.signature.payloadBytes();
    if ( pending.invocation_id == 0 || pending.invocation_id <= completed_invocation_ ||
            !payload_bytes || pending.source.data == nullptr ||
            pending.source.bytes != *payload_bytes || pending.result.data == nullptr ||
            pending.result.bytes != *payload_bytes ) {
        return CollectiveSubmitResult::Invalid;
    }
    if ( active_invocation_ != 0 || !owner_.collectiveCommandSlotAvailable() ) {
        return CollectiveSubmitResult::Retry;
    }

    StaticCollectiveContribution contribution;
    contribution.route = accepted_.route;
    contribution.invocation_id = pending.invocation_id;
    contribution.signature = pending.signature;
    contribution.value.resize(*payload_bytes);
    std::memcpy(contribution.value.data(), pending.source.data, contribution.value.size());
    auto event = std::make_unique<NicCollectiveSubmitCmdEvent>(
        pending.participant.physical_route, std::move(contribution));

    result_ = pending.result;
    active_signature_ = pending.signature;
    active_invocation_ = pending.invocation_id;
    awaiting_ack_invocation_ = pending.invocation_id;
    token_.emplace(pending.consumeAfterAcceptance());
    ready_armed_ = false;
    owner_.sendCollectiveCommand(event.release());
    return CollectiveSubmitResult::Accepted;
}

void FireflyCollectiveEndpoint::requestCollectiveReady(
    const AcceptedParticipantHandle& participant,
    const CollectiveSignatureV1& signature)
{
    if ( ready_ == nullptr || !sameParticipant(participant) || !supportsCollective(signature) ) return;
    ready_armed_ = true;
    ready_signature_ = signature;
    owner_.notifyReadyIfPossible();
}

void FireflyCollectiveEndpoint::submitAccepted(uint64_t invocation_id)
{
    if ( awaiting_ack_invocation_ == 0 || invocation_id != awaiting_ack_invocation_ ) {
        owner_.collectiveFatal("Mismatched or duplicate collective submit acceptance");
    }
    awaiting_ack_invocation_ = 0;
}

void FireflyCollectiveEndpoint::receiveResult(StaticCollectiveResult completed)
{
    if ( active_invocation_ == 0 || !completed.valid() ||
            completed.route != accepted_.route || completed.invocation_id != active_invocation_ ||
            completed.signature != active_signature_ || !token_ || completion_ == nullptr ||
            result_.data == nullptr || result_.bytes != completed.value.size() ) {
        owner_.collectiveFatal("Mismatched or duplicate collective result");
    }

    std::memcpy(result_.data, completed.value.data(), completed.value.size());
    CollectiveCompletionToken token(std::move(*token_));
    token_.reset();
    result_ = {};
    active_signature_ = {};
    completed_invocation_ = active_invocation_;
    active_invocation_ = 0;
    completion_->complete(std::move(token), CollectiveCompletionStatus::Success);
    owner_.notifyReadyIfPossible();
}

bool FireflyCollectiveEndpoint::notifyReadyIfPossible()
{
    if ( !ready_armed_ || active_invocation_ != 0 || ready_ == nullptr ||
            !supportsCollective(ready_signature_) || !owner_.collectiveCommandSlotAvailable() ) {
        return false;
    }
    ready_armed_ = false;
    ready_signature_ = {};
    ready_->ready(accepted_);
    return true;
}

} // namespace SST::Firefly
