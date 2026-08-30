// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

#include <sst_config.h>

#include "sst/elements/firefly/tests/allreduceErrorRegressionTest.h"
#include "sst/elements/firefly/funcSM/allreduce.h"
#include "sst/elements/firefly/funcSM/event.h"

namespace SST::Firefly {
namespace {

using namespace SST::Collective;

bool event_destroyed = false;

class TrackedCollectiveStartEvent final : public CollectiveStartEvent
{
public:
    TrackedCollectiveStartEvent(const Hermes::MemAddr& source,
            const Hermes::MemAddr& result) :
        CollectiveStartEvent(source, result, 1, MP::DOUBLE, MP::SUM, 0,
            MP::GroupWorld, CollectiveStartEvent::Allreduce)
    {}

    ~TrackedCollectiveStartEvent() override { event_destroyed = true; }
};

AcceptedParticipantHandle makeParticipant(ComponentId_t owner)
{
    AcceptedParticipantHandle participant;
    participant.route = {1, 1};
    participant.physical_route = {0, 1};
    participant.route_kind = CollectiveRouteKind::FabricTree;
    participant.data_mode = CollectiveDataMode::Functional;
    participant.physical_endpoint_id = 0;
    participant.local_participant_slot = 0;
    participant.local_participant_count = 1;
    participant.logical_participant_id = 0;
    participant.binding = {static_cast<uint64_t>(owner), 0, 1};
    participant.accepted_invocation_quota = 1;
    participant.submission_window = 1;
    participant.fabric.emplace();
    participant.fabric->endpoint_reduce_vn = 0;
    participant.fabric->endpoint_result_vn = 1;
    participant.fabric->injection_dest_nid = 0;
    return participant;
}

} // namespace

AllreduceErrorRegressionTest::AllreduceErrorRegressionTest(ComponentId_t id, Params& params) :
    Component(id)
{
    run_link_ = configureSelfLink("allreduce_error_run", "1ns",
        new SST::Event::Handler<AllreduceErrorRegressionTest,
            &AllreduceErrorRegressionTest::run>(this));
    if ( run_link_ == nullptr ) fail("could not configure self link");
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

void AllreduceErrorRegressionTest::setup()
{
    run_link_->send(1, nullptr);
}

void AllreduceErrorRegressionTest::run(SST::Event* event)
{
    delete event;
    using namespace SST::Collective;

    Params params;
    params.insert("name", "Allreduce");
    params.insert("nodeId", "0");
    AllreduceOffloadFuncSM allreduce(params);

    constexpr uint64_t invocation_id = 41;
    const AcceptedParticipantHandle participant = makeParticipant(getId());
    if ( !participant.valid() ) fail("invalid participant fixture");

    double source = 2.0;
    double result = 0.0;
    event_destroyed = false;
    allreduce.offload_event_ = new TrackedCollectiveStartEvent(
        Hermes::MemAddr(&source), Hermes::MemAddr(&result));
    allreduce.participant_ = &participant;
    allreduce.active_invocation_id_ = invocation_id;
    allreduce.mode_ = AllreduceOffloadFuncSM::Mode::WaitingCompletion;
    allreduce.pending_.participant = participant;
    allreduce.pending_.invocation_id = invocation_id;
    allreduce.pending_.operation = CollectiveOperation::Sum;
    allreduce.pending_.datatype = CollectiveDatatype::F64;
    allreduce.pending_.element_count = 1;
    allreduce.pending_.source = {
        reinterpret_cast<const uint8_t*>(&source), sizeof(source)};
    allreduce.pending_.result = {
        reinterpret_cast<uint8_t*>(&result), sizeof(result)};
    allreduce.pending_.completion = CollectiveCompletionToken(
        participant.binding.adapter_slot, invocation_id, participant.binding.generation);

    CollectiveCompletionToken accepted_token =
        allreduce.pending_.consumeAfterAcceptance();
    if ( allreduce.pending_.state != CollectivePendingState::Consumed ||
         allreduce.pending_.completion.valid() || !accepted_token.valid() ) {
        fail("accepted operation did not transfer completion ownership");
    }

    // The fixture owns the progress wakeup; this keeps complete() on its real
    // validation path without requiring an entire CtrlMsg/Ember driver stack.
    allreduce.wake_scheduled_ = true;
    allreduce.complete(
        std::move(accepted_token), CollectiveCompletionStatus::RecoverableError);
    if ( accepted_token.valid() || !allreduce.completion_received_ ||
         allreduce.completion_status_ != CollectiveCompletionStatus::RecoverableError ) {
        fail("RecoverableError completion was not accepted exactly once");
    }

    FunctionSMInterface::Retval retval;
    allreduce.handleEnterEvent(retval);
    if ( !retval.isExit() || retval.value() != 1 || !event_destroyed ||
         allreduce.offload_event_ != nullptr || allreduce.active_invocation_id_ != 0 ||
         allreduce.mode_ != AllreduceOffloadFuncSM::Mode::Idle || allreduce.ready_received_ ||
         allreduce.completion_received_ || allreduce.wake_scheduled_ ||
         allreduce.pending_.completion.valid() ||
         allreduce.pending_.state != CollectivePendingState::Ready ||
         allreduce.pending_.invocation_id != 0 || allreduce.pending_.source.data != nullptr ||
         allreduce.pending_.result.data != nullptr ) {
        fail("terminal failure did not clear invocation ownership and state");
    }

    getSimulationOutput().output(
        "Firefly Allreduce accepted RecoverableError: retval=%" PRIu64 " state=cleared PASS\n",
        retval.value());
    primaryComponentOKToEndSim();
}

[[noreturn]] void AllreduceErrorRegressionTest::fail(const char* reason) const
{
    getSimulationOutput().fatal(CALL_INFO, 1,
        "Firefly Allreduce RecoverableError regression failed: %s\n", reason);
}

} // namespace SST::Firefly
