// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.

#include <sst_config.h>

#include "sst/elements/firefly/funcSM/allreduce.h"
#include "sst/elements/firefly/funcSM/event.h"

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/link.h>

namespace SST::Firefly {

using SimpleNetwork = SST::Interfaces::SimpleNetwork;

class EmptyRequestRegressionTest final : public SST::Component
{
public:
    SST_ELI_REGISTER_COMPONENT(EmptyRequestRegressionTest, "firefly",
        "empty_request_regression_test", SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Send an untagged empty Request through a Firefly NIC", COMPONENT_CATEGORY_NETWORK)
    SST_ELI_DOCUMENT_PARAMS()
    SST_ELI_DOCUMENT_PORTS({ "nic", "Firefly NIC initialization link", {} })
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "networkIF", "Request injector network interface", "SST::Interfaces::SimpleNetwork" })

    EmptyRequestRegressionTest(ComponentId_t id, Params&) : Component(id)
    {
        network_ = loadUserSubComponent<SimpleNetwork>("networkIF", ComponentInfo::SHARE_NONE, 1);
        host_ = configureLink("nic");
        run_ = configureSelfLink("run", "1ns",
            new SST::Event::Handler<EmptyRequestRegressionTest,
                &EmptyRequestRegressionTest::run>(this));
        if ( network_ == nullptr || host_ == nullptr || run_ == nullptr ) fail("configuration");
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
    }

    void init(unsigned phase) override
    {
        network_->init(phase);
        while ( auto* event = host_->recvUntimedData() ) {
            delete event;
            if ( initialized_ ) fail("duplicate NIC initialization");
            initialized_ = true;
        }
    }

    void setup() override
    {
        network_->setup();
        if ( !initialized_ ) fail("missing NIC initialization");
        run_->send(1, nullptr);
    }

    void finish() override { network_->finish(); }

private:
    void run(SST::Event* event)
    {
        delete event;
        if ( sent_ ) {
            getSimulationOutput().output("Firefly untagged empty Request PASS\n");
            primaryComponentOKToEndSim();
            return;
        }
        if ( !network_->isNetworkInitialized() ) fail("network not initialized");
        auto* request = new SimpleNetwork::Request(1, network_->getEndpointID(), 64, true, true);
        request->vn = 0;
        if ( request->inspectPayload() != nullptr || request->hasService() ||
             !network_->send(request, 0) ) {
            delete request;
            fail("empty Request injection");
        }
        sent_ = true;
        run_->send(200, nullptr);
    }

    [[noreturn]] void fail(const char* reason) const
    {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "Firefly empty Request regression failed: %s\n", reason);
    }

    SimpleNetwork* network_ = nullptr;
    SST::Link*     host_ = nullptr;
    SST::Link*     run_ = nullptr;
    bool           initialized_ = false;
    bool           sent_ = false;
};

namespace {

using namespace SST::Collective;
bool event_destroyed = false;

class TrackedCollectiveStartEvent final : public CollectiveStartEvent
{
public:
    TrackedCollectiveStartEvent(const Hermes::MemAddr& source, const Hermes::MemAddr& result) :
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
    participant.local_participant_count = 1;
    participant.binding = {static_cast<uint64_t>(owner), 0, 1};
    participant.accepted_invocation_quota = participant.submission_window = 1;
    participant.fabric = FabricParticipantRouteV1{0, 1, 0};
    return participant;
}

} // namespace

class AllreduceErrorRegressionTest final : public SST::Component
{
public:
    SST_ELI_REGISTER_COMPONENT(AllreduceErrorRegressionTest, "firefly",
        "allreduce_error_regression_test", SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Check terminal cleanup after RecoverableError", COMPONENT_CATEGORY_PROCESSOR)
    SST_ELI_DOCUMENT_PARAMS()
    SST_ELI_DOCUMENT_PORTS()

    AllreduceErrorRegressionTest(ComponentId_t id, Params&) : Component(id)
    {
        run_ = configureSelfLink("run", "1ns",
            new SST::Event::Handler<AllreduceErrorRegressionTest,
                &AllreduceErrorRegressionTest::run>(this));
        if ( run_ == nullptr ) fail("configuration");
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
    }

    void setup() override { run_->send(1, nullptr); }

private:
    void run(SST::Event* event)
    {
        delete event;
        using namespace SST::Collective;
        Params params;
        params.insert("name", "Allreduce");
        params.insert("nodeId", "0");
        AllreduceOffloadFuncSM allreduce(params);
        const AcceptedParticipantHandle participant = makeParticipant(getId());
        constexpr uint64_t invocation = 41;
        double source = 2.0;
        double result = 0.0;

        event_destroyed = false;
        allreduce.offload_event_ = new TrackedCollectiveStartEvent(
            Hermes::MemAddr(&source), Hermes::MemAddr(&result));
        allreduce.participant_ = &participant;
        allreduce.active_invocation_id_ = invocation;
        allreduce.mode_ = AllreduceOffloadFuncSM::Mode::WaitingCompletion;
        auto& pending = allreduce.pending_;
        pending.participant = participant;
        pending.invocation_id = invocation;
        pending.operation = CollectiveOperation::Sum;
        pending.datatype = CollectiveDatatype::F64;
        pending.element_count = 1;
        pending.source = {reinterpret_cast<const uint8_t*>(&source), sizeof(source)};
        pending.result = {reinterpret_cast<uint8_t*>(&result), sizeof(result)};
        pending.completion = CollectiveCompletionToken(0, invocation, 1);
        CollectiveCompletionToken token = pending.consumeAfterAcceptance();

        allreduce.wake_scheduled_ = true;
        allreduce.complete(std::move(token), CollectiveCompletionStatus::RecoverableError);
        FunctionSMInterface::Retval retval;
        allreduce.handleEnterEvent(retval);
        if ( token.valid() || !retval.isExit() || retval.value() != 1 || !event_destroyed ||
             allreduce.offload_event_ != nullptr || allreduce.active_invocation_id_ != 0 ||
             allreduce.mode_ != AllreduceOffloadFuncSM::Mode::Idle ||
             allreduce.ready_received_ || allreduce.completion_received_ ||
             allreduce.wake_scheduled_ || pending.completion.valid() ||
             pending.state != CollectivePendingState::Ready || pending.invocation_id != 0 ||
             pending.source.data != nullptr || pending.result.data != nullptr ) {
            fail("terminal state was retained");
        }
        getSimulationOutput().output("Firefly RecoverableError cleanup PASS\n");
        primaryComponentOKToEndSim();
    }

    [[noreturn]] void fail(const char* reason) const
    {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "Firefly RecoverableError regression failed: %s\n", reason);
    }

    SST::Link* run_ = nullptr;
};

} // namespace SST::Firefly
