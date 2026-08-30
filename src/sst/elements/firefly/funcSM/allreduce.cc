// Copyright 2013-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2013-2026, NTESS
// All rights reserved.

#include <sst_config.h>

#include "funcSM/allreduce.h"
#include "ctrlMsg.h"
#include "info.h"

#include <cstdio>

namespace SST::Firefly {
namespace {

using namespace SST::Collective;

bool sameParticipantIdentity(
    const AcceptedParticipantHandle& lhs, const AcceptedParticipantHandle& rhs)
{
    return lhs.route == rhs.route && lhs.physical_route == rhs.physical_route &&
        lhs.binding == rhs.binding && lhs.local_participant_slot == rhs.local_participant_slot;
}

} // namespace

AllreduceOffloadFuncSM::AllreduceOffloadFuncSM(SST::Params& params) :
    CollectiveTreeFuncSM(params),
    enable_offload_(params.find<bool>("enableOffload", false)),
    force_software_(params.find<bool>("forceSoftware", false)),
    report_offload_(params.find<bool>("reportOffload", false))
{}

AllreduceOffloadFuncSM::~AllreduceOffloadFuncSM()
{
    delete offload_event_;
}

CtrlMsg::API* AllreduceOffloadFuncSM::collectiveProtocol() const
{
    return static_cast<CtrlMsg::API*>(m_proto);
}

bool AllreduceOffloadFuncSM::eligible(const CollectiveStartEvent& event) const
{
    return event.type == CollectiveStartEvent::Allreduce && event.group == MP::GroupWorld &&
        event.root == 0 && event.count == 1 && event.dtype == MP::DOUBLE && event.op != nullptr &&
        event.op->type == MP::ReductionOpType::Sum && event.mydata.getBacking() != nullptr &&
        event.result.getBacking() != nullptr;
}

void AllreduceOffloadFuncSM::reportPath(const char* path) const
{
    if ( report_offload_ ) {
        std::printf("Firefly Allreduce rank %d %s\n", m_info->worldRank(), path);
    }
}

void AllreduceOffloadFuncSM::fail(const char* reason)
{
    m_dbg.fatal(CALL_INFO, -1, "Firefly Allreduce offload: %s\n", reason);
}

void AllreduceOffloadFuncSM::startSoftware(CollectiveStartEvent* event, Retval& retval)
{
    mode_ = Mode::Software;
    reportPath("SOFTWARE FALLBACK");
    CollectiveTreeFuncSM::handleStartEvent(event, retval);
    if ( retval.isExit() ) mode_ = Mode::Idle;
}

bool AllreduceOffloadFuncSM::bindOffload()
{
    auto* protocol = collectiveProtocol();
    if ( protocol == nullptr ) return false;
    auto* endpoint = protocol->collectiveEndpoint();
    const auto* participant = protocol->collectiveParticipant(0);
    if ( endpoint == nullptr || participant == nullptr || !participant->valid() ||
            participant->route_kind != CollectiveRouteKind::FabricTree ||
            participant->data_mode != CollectiveDataMode::Functional ||
            participant->local_participant_slot != 0 || participant->local_participant_count != 1 ||
            participant->logical_participant_id != static_cast<uint64_t>(m_info->worldRank()) ||
            participant->accepted_invocation_quota != 1 || participant->submission_window != 1 ) {
        return false;
    }

    if ( !bound_ ) {
        if ( !endpoint->bindParticipant(*participant, *this, *this) ) return false;
        endpoint_ = endpoint;
        participant_ = participant;
        bound_ = true;
    }
    return endpoint_ == endpoint && participant_ == participant;
}

void AllreduceOffloadFuncSM::startOffload(
    CollectiveStartEvent* event, uint64_t invocation_id, Retval& retval)
{
    offload_event_ = event;
    active_invocation_id_ = invocation_id;
    completion_status_ = CollectiveCompletionStatus::RecoverableError;
    completion_received_ = false;
    ready_received_ = false;
    wake_scheduled_ = false;

    pending_ = CollectivePending {};
    pending_.participant = *participant_;
    pending_.invocation_id = invocation_id;
    pending_.operation = CollectiveOperation::Sum;
    pending_.datatype = CollectiveDatatype::F64;
    pending_.element_count = 1;
    pending_.source = {reinterpret_cast<const uint8_t*>(event->mydata.getBacking()), sizeof(double)};
    pending_.result = {reinterpret_cast<uint8_t*>(event->result.getBacking()), sizeof(double)};
    pending_.completion = CollectiveCompletionToken(participant_->binding.adapter_slot,
        invocation_id, participant_->binding.generation);

    submitOffload(retval);
}

void AllreduceOffloadFuncSM::submitOffload(Retval& retval)
{
    mode_ = Mode::WaitingCompletion;
    const auto result = endpoint_->trySubmitCollective(pending_);
    if ( result == CollectiveSubmitResult::Accepted ) {
        if ( pending_.state != CollectivePendingState::Consumed || pending_.completion.valid() ) {
            fail("endpoint accepted without consuming completion ownership");
        }
        reportPath("OFFLOAD ACCEPTED");
        return;
    }

    if ( !pending_.readyForSubmit() ) {
        fail("endpoint consumed ownership without accepting");
    }
    if ( result == CollectiveSubmitResult::Unsupported ) {
        CollectiveStartEvent* event = offload_event_;
        offload_event_ = nullptr;
        active_invocation_id_ = 0;
        pending_ = CollectivePending {};
        startSoftware(event, retval);
        return;
    }
    if ( result == CollectiveSubmitResult::Invalid ) {
        fail("endpoint rejected a valid POC submission");
    }
    if ( result != CollectiveSubmitResult::Retry ) {
        fail("endpoint returned an unknown submit result");
    }

    mode_ = Mode::WaitingReady;
    ready_received_ = false;
    endpoint_->requestCollectiveReady(*participant_);
}

void AllreduceOffloadFuncSM::scheduleResume()
{
    if ( wake_scheduled_ ) return;
    if ( !collectiveProtocol()->resumeCollectiveFunction() ) {
        fail("unable to schedule FunctionSM progress");
    }
    wake_scheduled_ = true;
}

void AllreduceOffloadFuncSM::finishOffload(Retval& retval)
{
    const bool terminal_error =
        completion_status_ == CollectiveCompletionStatus::RecoverableError;
    delete offload_event_;
    offload_event_ = nullptr;
    pending_ = CollectivePending {};
    active_invocation_id_ = 0;
    ready_received_ = false;
    completion_received_ = false;
    wake_scheduled_ = false;
    mode_ = Mode::Idle;
    // Once the fabric accepted the operation it is unsafe for just one rank to
    // enter the software collective.  Treat the transport's "recoverable"
    // status as terminal for this MPI call after releasing all local ownership.
    if ( terminal_error ) reportPath("OFFLOAD TERMINAL ERROR");
    retval.setExit(terminal_error ? 1 : 0);
}

void AllreduceOffloadFuncSM::handleStartEvent(SST::Event* event, Retval& retval)
{
    if ( mode_ != Mode::Idle || event == nullptr ) fail("overlapping or null start event");
    auto* collective = static_cast<CollectiveStartEvent*>(event);

    uint64_t invocation_id = 0;
    if ( collective->type == CollectiveStartEvent::Allreduce && collective->group == MP::GroupWorld ) {
        invocation_id = next_invocation_id_++;
        if ( invocation_id == 0 || next_invocation_id_ == 0 ) {
            fail("invocation sequence exhausted");
        }
    }

    if ( !enable_offload_ || force_software_ || !eligible(*collective) ) {
        startSoftware(collective, retval);
        return;
    }
    if ( !bindOffload() ) {
        fail("enableOffload=true but no validated collective service route is available");
    }
    startOffload(collective, invocation_id, retval);
}

void AllreduceOffloadFuncSM::handleEnterEvent(Retval& retval)
{
    if ( mode_ == Mode::Software ) {
        CollectiveTreeFuncSM::handleEnterEvent(retval);
        if ( retval.isExit() ) mode_ = Mode::Idle;
        return;
    }

    wake_scheduled_ = false;
    if ( mode_ == Mode::WaitingReady ) {
        if ( !ready_received_ ) fail("resumed before a ready notification");
        ready_received_ = false;
        submitOffload(retval);
        return;
    }
    if ( mode_ == Mode::WaitingCompletion ) {
        if ( !completion_received_ ) fail("resumed before collective completion");
        finishOffload(retval);
        return;
    }
    fail("unexpected FunctionSM enter event");
}

void AllreduceOffloadFuncSM::complete(
    CollectiveCompletionToken&& token, CollectiveCompletionStatus status)
{
    if ( mode_ != Mode::WaitingCompletion || completion_received_ || !participant_ ||
            !isValid(status) || !token.valid() ||
            token.adapterSlot() != participant_->binding.adapter_slot ||
            token.generation() != participant_->binding.generation ||
            token.nativeRequestId() != active_invocation_id_ ) {
        fail("invalid or duplicate completion");
    }
    CollectiveCompletionToken returned_token(std::move(token));
    completion_status_ = status;
    completion_received_ = true;
    scheduleResume();
}

void AllreduceOffloadFuncSM::ready(const AcceptedParticipantHandle& participant)
{
    if ( mode_ != Mode::WaitingReady || ready_received_ || !participant_ ||
            !sameParticipantIdentity(participant, *participant_) ) {
        fail("invalid or duplicate ready notification");
    }
    ready_received_ = true;
    scheduleResume();
}

} // namespace SST::Firefly
