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
    if ( pending.operation != CollectiveOperation::Sum ||
            pending.datatype != CollectiveDatatype::F64 || pending.element_count != 1 ) {
        return CollectiveSubmitResult::Unsupported;
    }
    if ( pending.invocation_id == 0 || pending.invocation_id <= completed_invocation_ ||
            pending.source.data == nullptr ||
            pending.source.bytes != FIREFLY_COLLECTIVE_LOGICAL_BYTES ||
            pending.result.data == nullptr ||
            pending.result.bytes != FIREFLY_COLLECTIVE_LOGICAL_BYTES ) {
        return CollectiveSubmitResult::Invalid;
    }
    if ( active_invocation_ != 0 || !owner_.collectiveCommandSlotAvailable() ) {
        return CollectiveSubmitResult::Retry;
    }

    std::array<uint8_t, FIREFLY_COLLECTIVE_LOGICAL_BYTES> contribution {};
    std::memcpy(contribution.data(), pending.source.data, contribution.size());
    auto event = std::make_unique<NicCollectiveSubmitCmdEvent>(
        pending.participant.physical_route, pending.invocation_id, contribution);

    result_ = pending.result;
    active_invocation_ = pending.invocation_id;
    awaiting_ack_invocation_ = pending.invocation_id;
    token_.emplace(pending.consumeAfterAcceptance());
    ready_armed_ = false;
    owner_.sendCollectiveCommand(event.release());
    return CollectiveSubmitResult::Accepted;
}

void FireflyCollectiveEndpoint::requestCollectiveReady(
    const AcceptedParticipantHandle& participant)
{
    if ( ready_ == nullptr || !sameParticipant(participant) ) return;
    ready_armed_ = true;
    owner_.notifyReadyIfPossible();
}

void FireflyCollectiveEndpoint::submitAccepted(uint64_t invocation_id)
{
    if ( awaiting_ack_invocation_ == 0 || invocation_id != awaiting_ack_invocation_ ) {
        owner_.collectiveFatal("Mismatched or duplicate collective submit acceptance");
    }
    awaiting_ack_invocation_ = 0;
}

void FireflyCollectiveEndpoint::receiveResult(uint64_t invocation_id,
    const std::array<uint8_t, FIREFLY_COLLECTIVE_LOGICAL_BYTES>& bytes)
{
    if ( active_invocation_ == 0 || invocation_id != active_invocation_ || !token_ ||
            completion_ == nullptr || result_.data == nullptr ||
            result_.bytes != FIREFLY_COLLECTIVE_LOGICAL_BYTES ) {
        owner_.collectiveFatal("Mismatched or duplicate collective result");
    }

    std::memcpy(result_.data, bytes.data(), bytes.size());
    CollectiveCompletionToken token(std::move(*token_));
    token_.reset();
    result_ = {};
    completed_invocation_ = active_invocation_;
    active_invocation_ = 0;
    completion_->complete(std::move(token), CollectiveCompletionStatus::Success);
    owner_.notifyReadyIfPossible();
}

bool FireflyCollectiveEndpoint::notifyReadyIfPossible()
{
    if ( !ready_armed_ || active_invocation_ != 0 || ready_ == nullptr ||
            !owner_.collectiveCommandSlotAvailable() ) {
        return false;
    }
    ready_armed_ = false;
    ready_->ready(accepted_);
    return true;
}

} // namespace SST::Firefly
