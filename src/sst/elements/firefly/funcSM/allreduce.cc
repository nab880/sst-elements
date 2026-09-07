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
    const auto* participant = endpoint == nullptr ? nullptr : endpoint->participant();
    if ( participant == nullptr || !participant->valid() ||
            participant->logical_participant_id != static_cast<int64_t>(m_info->worldRank()) ) {
        return false;
    }

    if ( !bound_ ) {
        if ( !endpoint->bind(*this, *this) ) return false;
        endpoint_ = endpoint;
        bound_ = true;
    }
    return endpoint_ == endpoint;
}

void AllreduceOffloadFuncSM::startOffload(
    CollectiveStartEvent* event, const CollectiveSignatureV1& signature,
    uint64_t invocation_id, Retval& retval)
{
    const auto payload_bytes = signature.payloadBytes();
    if ( !payload_bytes ) fail("translated signature has no valid payload size");

    offload_event_ = event;
    active_invocation_id_ = invocation_id;
    completion_received_ = false;
    ready_received_ = false;
    wake_scheduled_ = false;

    submission_ = CollectiveSubmission {};
    submission_.invocation_id = invocation_id;
    submission_.signature = signature;
    submission_.source = {reinterpret_cast<const uint8_t*>(event->mydata.getBacking()), *payload_bytes};
    submission_.result = {reinterpret_cast<uint8_t*>(event->result.getBacking()), *payload_bytes};

    submitOffload(retval);
}

void AllreduceOffloadFuncSM::submitOffload(Retval& retval)
{
    mode_ = Mode::WaitingCompletion;
    const auto result = collectiveProtocol()->submitCollective(submission_,
        offload_event_->mydata.getSimVAddr(), offload_event_->result.getSimVAddr());
    if ( result == CollectiveSubmitResult::Accepted ) {
        reportPath("OFFLOAD ACCEPTED");
        return;
    }

    if ( result == CollectiveSubmitResult::Unsupported ) {
        CollectiveStartEvent* event = offload_event_;
        offload_event_ = nullptr;
        active_invocation_id_ = 0;
        submission_ = CollectiveSubmission {};
        startSoftware(event, retval);
        return;
    }
    if ( result == CollectiveSubmitResult::Invalid ) {
        fail("endpoint rejected a valid submission");
    }
    if ( result != CollectiveSubmitResult::Retry ) {
        fail("endpoint returned an unknown submit result");
    }

    mode_ = Mode::WaitingReady;
    ready_received_ = false;
    endpoint_->requestCollectiveReady(submission_.signature);
}

void AllreduceOffloadFuncSM::scheduleResume()
{
    if ( wake_scheduled_ ) return;
    if ( !collectiveProtocol()->resumeCollectiveFunction() ) {
        fail("unable to schedule FunctionSM progress");
    }
    wake_scheduled_ = true;
}

void AllreduceOffloadFuncSM::finishOffload()
{
    delete offload_event_;
    offload_event_ = nullptr;
    submission_ = CollectiveSubmission {};
    active_invocation_id_ = 0;
    ready_received_ = false;
    completion_received_ = false;
    wake_scheduled_ = false;
    mode_ = Mode::Idle;
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
        finishOffload();
        retval.setExit(0);
        return;
    }
    fail("unexpected FunctionSM enter event");
}

void AllreduceOffloadFuncSM::complete(uint64_t invocation_id, CollectiveCompletionStatus status)
{
    if ( mode_ != Mode::WaitingCompletion || completion_received_ || !isValid(status) ||
            invocation_id == 0 || invocation_id != active_invocation_id_ ) {
        fail("invalid or duplicate completion");
    }
    completion_received_ = true;
    scheduleResume();
}

void AllreduceOffloadFuncSM::ready()
{
    if ( mode_ != Mode::WaitingReady || ready_received_ ) {
        fail("invalid or duplicate ready notification");
    }
    ready_received_ = true;
    scheduleResume();
}

} // namespace SST::Firefly
