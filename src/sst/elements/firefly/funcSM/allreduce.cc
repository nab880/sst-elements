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
#include <limits>
#include <optional>

namespace SST::Firefly {
namespace {

using namespace SST::Collective;

bool sameParticipantIdentity(
    const AcceptedParticipantHandle& lhs, const AcceptedParticipantHandle& rhs)
{
    return lhs.route == rhs.route && lhs.physical_route == rhs.physical_route &&
        lhs.binding == rhs.binding && lhs.local_participant_slot == rhs.local_participant_slot;
}

std::optional<CollectiveOperation>
translateOperation(MP::ReductionOperation operation)
{
    if ( operation == nullptr ) return std::nullopt;
    switch ( operation->type ) {
    case MP::ReductionOpType::Sum:
        return CollectiveOperation::Sum;
    case MP::ReductionOpType::Min:
        return CollectiveOperation::Min;
    case MP::ReductionOpType::Max:
        return CollectiveOperation::Max;
    case MP::ReductionOpType::Nop:
    case MP::ReductionOpType::Func:
        return std::nullopt;
    }
    return std::nullopt;
}

struct TranslatedDatatype
{
    CollectiveDatatype datatype;
    uint64_t            native_bytes;
};

std::optional<TranslatedDatatype>
signedIntegerDatatype(uint64_t native_bytes)
{
    if ( native_bytes == 4 ) return TranslatedDatatype { CollectiveDatatype::I32, native_bytes };
    if ( native_bytes == 8 ) return TranslatedDatatype { CollectiveDatatype::I64, native_bytes };
    return std::nullopt;
}

std::optional<TranslatedDatatype>
unsignedIntegerDatatype(uint64_t native_bytes)
{
    if ( native_bytes == 4 ) return TranslatedDatatype { CollectiveDatatype::U32, native_bytes };
    if ( native_bytes == 8 ) return TranslatedDatatype { CollectiveDatatype::U64, native_bytes };
    return std::nullopt;
}

std::optional<TranslatedDatatype>
translateDatatype(MP::PayloadDataType datatype)
{
    switch ( datatype ) {
    case MP::SIGNED_CHAR:
        return signedIntegerDatatype(sizeof(signed char));
    case MP::INT:
        return signedIntegerDatatype(sizeof(int));
    case MP::LONG:
        return signedIntegerDatatype(sizeof(long));
    case MP::LONG_LONG:
        return signedIntegerDatatype(sizeof(long long));
    case MP::INT8_T:
        return signedIntegerDatatype(sizeof(std::int8_t));
    case MP::INT16_T:
        return signedIntegerDatatype(sizeof(std::int16_t));
    case MP::INT32_T:
        return signedIntegerDatatype(sizeof(std::int32_t));
    case MP::INT64_T:
        return signedIntegerDatatype(sizeof(std::int64_t));
    case MP::UNSIGNED_CHAR:
        return unsignedIntegerDatatype(sizeof(unsigned char));
    case MP::UNSIGNED_INT:
        return unsignedIntegerDatatype(sizeof(unsigned int));
    case MP::UNSIGNED_LONG:
        return unsignedIntegerDatatype(sizeof(unsigned long));
    case MP::UNSIGNED_LONG_LONG:
        return unsignedIntegerDatatype(sizeof(unsigned long long));
    case MP::UINT8_T:
        return unsignedIntegerDatatype(sizeof(std::uint8_t));
    case MP::UINT16_T:
        return unsignedIntegerDatatype(sizeof(std::uint16_t));
    case MP::UINT32_T:
        return unsignedIntegerDatatype(sizeof(std::uint32_t));
    case MP::UINT64_T:
        return unsignedIntegerDatatype(sizeof(std::uint64_t));
    case MP::FLOAT:
        if ( sizeof(float) == 4 ) return TranslatedDatatype { CollectiveDatatype::F32, sizeof(float) };
        return std::nullopt;
    case MP::DOUBLE:
        if ( sizeof(double) == 8 ) return TranslatedDatatype { CollectiveDatatype::F64, sizeof(double) };
        return std::nullopt;
    case MP::CHAR:
    case MP::COMPLEX:
        return std::nullopt;
    }
    return std::nullopt;
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

std::optional<CollectiveSignatureV1>
AllreduceOffloadFuncSM::translateSignature(const CollectiveStartEvent& event)
{
    if ( event.type != CollectiveStartEvent::Allreduce || event.group != MP::GroupWorld ||
         event.root != 0 || event.count == 0 || event.mydata.getBacking() == nullptr ||
         event.result.getBacking() == nullptr ) {
        return std::nullopt;
    }

    const auto operation = translateOperation(event.op);
    const auto datatype  = translateDatatype(event.dtype);
    if ( !operation || !datatype ) return std::nullopt;

    CollectiveSignatureV1 signature { *operation, datatype->datatype, event.count };
    const auto payload_bytes = signature.payloadBytes();
    if ( !payload_bytes || datatype->native_bytes >
            std::numeric_limits<uint64_t>::max() / event.count ||
         *payload_bytes != datatype->native_bytes * event.count ) {
        return std::nullopt;
    }
    return signature;
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
    CollectiveStartEvent* event, const CollectiveSignatureV1& signature,
    uint64_t invocation_id, Retval& retval)
{
    const auto payload_bytes = signature.payloadBytes();
    if ( !payload_bytes ) fail("translated signature has no valid payload size");

    offload_event_ = event;
    active_invocation_id_ = invocation_id;
    completion_status_ = CollectiveCompletionStatus::RecoverableError;
    completion_received_ = false;
    ready_received_ = false;
    wake_scheduled_ = false;

    pending_ = CollectivePending {};
    pending_.participant = *participant_;
    pending_.invocation_id = invocation_id;
    pending_.signature = signature;
    pending_.source = {reinterpret_cast<const uint8_t*>(event->mydata.getBacking()), *payload_bytes};
    pending_.result = {reinterpret_cast<uint8_t*>(event->result.getBacking()), *payload_bytes};
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
    endpoint_->requestCollectiveReady(*participant_, pending_.signature);
}

void AllreduceOffloadFuncSM::scheduleResume()
{
    if ( wake_scheduled_ ) return;
    if ( !collectiveProtocol()->resumeCollectiveFunction() ) {
        fail("unable to schedule FunctionSM progress");
    }
    wake_scheduled_ = true;
}

CollectiveStartEvent* AllreduceOffloadFuncSM::finishOffload()
{
    const bool recoverable =
        completion_status_ == CollectiveCompletionStatus::RecoverableError;
    CollectiveStartEvent* restart_event = recoverable ? offload_event_ : nullptr;
    if ( !recoverable ) delete offload_event_;
    offload_event_ = nullptr;
    pending_ = CollectivePending {};
    active_invocation_id_ = 0;
    ready_received_ = false;
    completion_received_ = false;
    wake_scheduled_ = false;
    mode_ = Mode::Idle;
    return restart_event;
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

    const auto signature = translateSignature(*collective);
    if ( !enable_offload_ || force_software_ || !signature ) {
        startSoftware(collective, retval);
        return;
    }
    auto* protocol = collectiveProtocol();
    auto* candidate_endpoint = protocol == nullptr ? nullptr : protocol->collectiveEndpoint();
    if ( candidate_endpoint != nullptr && !candidate_endpoint->supportsCollective(*signature) ) {
        startSoftware(collective, retval);
        return;
    }
    if ( !bindOffload() ) {
        fail("enableOffload=true but no validated collective service route is available");
    }
    startOffload(collective, *signature, invocation_id, retval);
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
        if ( auto* restart_event = finishOffload() ) {
            startSoftware(restart_event, retval);
        }
        else {
            retval.setExit(0);
        }
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
