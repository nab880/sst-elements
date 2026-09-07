// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "sst_config.h"

#include "networkServiceTest.h"

#include "sst/elements/merlin/hr_router/hr_router.h"
#include "sst/elements/merlin/hr_router/xbar_arb_rr.h"
#include "sst/elements/merlin/hr_router/xbar_arb_lru.h"
#include "sst/elements/merlin/interfaces/ExtendedRequest.h"
#include "sst/elements/merlin/networkService.h"
#include "sst/elements/merlin/test/network_service/pr2IntegrationFixture.h"

#include <sst/core/interfaces/stringEvent.h>
#include <sst/core/output.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace SST::Merlin {
namespace {

void
require(bool condition, const char* message)
{
    if ( !condition ) throw std::runtime_error(message);
}

class CloneTrackingEvent final : public SST::Event
{
public:
    CloneTrackingEvent(int& clones, int& destructions, bool return_null = false, bool throw_on_clone = false) :
        clones_(&clones),
        destructions_(&destructions),
        return_null_(return_null),
        throw_on_clone_(throw_on_clone)
    {}

    ~CloneTrackingEvent() override { ++*destructions_; }

    SST::Event* clone() override
    {
        ++*clones_;
        if ( throw_on_clone_ ) throw std::runtime_error("test clone failure");
        if ( return_null_ ) return nullptr;
        return new CloneTrackingEvent(*clones_, *destructions_);
    }

private:
    int* clones_;
    int* destructions_;
    bool return_null_;
    bool throw_on_clone_;
};

class FakeXbarPort final : public PortInterface
{
public:
    void setHead(internal_router_event* event) { heads_[0] = event; }
    void setCredits(bool credits) { credits_ = credits; }

    void recvCtrlEvent(CtrlRtrEvent*) override {}
    void sendCtrlEvent(CtrlRtrEvent*) override {}
    void send(internal_router_event*, int) override {}
    bool spaceToSend(int vc, int flits) override { return credits_ && vc == 0 && flits == 1; }
    internal_router_event* recv(int) override { return nullptr; }
    internal_router_event** getVCHeads() override { return heads_; }
    void reportIncomingEvent(internal_router_event*) override {}
    void initVCs(int, int*, internal_router_event**, int*, int*) override {}
    void sendUntimedData(Event* event) override { delete event; }
    Event* recvUntimedData() override { return nullptr; }
    bool decreaseLinkWidth() override { return false; }
    bool increaseLinkWidth() override { return false; }

private:
    internal_router_event* heads_[1] = { nullptr };
    bool credits_ = true;
};

class FakeXbarPort2 final : public PortInterface
{
public:
    void setHead(int vc, internal_router_event* event) { heads_[vc] = event; }

    void recvCtrlEvent(CtrlRtrEvent*) override {}
    void sendCtrlEvent(CtrlRtrEvent*) override {}
    void send(internal_router_event*, int) override {}
    bool spaceToSend(int vc, int flits) override { return vc >= 0 && vc < 2 && flits == 1; }
    internal_router_event* recv(int) override { return nullptr; }
    internal_router_event** getVCHeads() override { return heads_; }
    void reportIncomingEvent(internal_router_event*) override {}
    void initVCs(int, int*, internal_router_event**, int*, int*) override {}
    void sendUntimedData(Event* event) override { delete event; }
    Event* recvUntimedData() override { return nullptr; }
    bool decreaseLinkWidth() override { return false; }
    bool increaseLinkWidth() override { return false; }

private:
    internal_router_event* heads_[2] = { nullptr, nullptr };
};

std::unique_ptr<internal_router_event>
makeArbitrationEvent(int next_port = 0)
{
    auto* request = new SST::Interfaces::SimpleNetwork::Request(0, 0, 64, true, true);
    request->vn = 0;
    auto* envelope = new RtrEvent(request, 0, 0);
    require(envelope->setSyntheticTransportMetadata(1, 0), "could not create arbitration envelope");
    auto event = std::make_unique<internal_router_event>(envelope);
    event->setNextPort(next_port);
    event->setVC(0);
    return event;
}

void
testDispositions()
{
    const NetworkServiceDecision accept { NetworkServiceDisposition::Accept, 0x11 };
    const NetworkServiceDecision busy { NetworkServiceDisposition::Busy, 0x22 };
    require(isValid(accept.disposition) && accept.opaque_diagnostic == 0x11,
        "ACCEPT decision did not preserve its disposition or diagnostic");
    require(isValid(busy.disposition) && busy.opaque_diagnostic == 0x22,
        "BUSY decision did not preserve its disposition or diagnostic");
    require(isValid(NetworkServiceDisposition::Reject), "REJECT disposition was rejected");
    require(!isValid(static_cast<NetworkServiceDisposition>(0)) &&
            !isValid(static_cast<NetworkServiceDisposition>(4)),
        "invalid dispositions were accepted");
}

void
testBoundedSyntheticRequester()
{
    NetworkServiceSyntheticRequester requester(2, 3);

    auto first = makeArbitrationEvent();
    auto second = makeArbitrationEvent();
    auto third = makeArbitrationEvent();
    auto blocked = makeArbitrationEvent();
    third->setVC(1);
    blocked->setVC(1);
    internal_router_event* first_raw = first.get();
    internal_router_event* second_raw = second.get();
    internal_router_event* third_raw = third.get();
    internal_router_event* blocked_raw = blocked.get();

    require(requester.enqueue(first, 0) && !first, "first synthetic enqueue did not consume ownership");
    require(requester.enqueue(second, 0) && !second, "second synthetic enqueue did not consume ownership");
    require(requester.enqueue(third, 1) && !third, "third synthetic enqueue did not consume ownership");
    require(requester.size() == 3 && requester.hasWork(), "synthetic requester size is incorrect at capacity");
    require(requester.getVCHeads()[0] == first_raw && requester.getVCHeads()[1] == third_raw,
        "synthetic requester exposed the wrong VC heads");

    require(!requester.canEnqueue(0) && !requester.enqueue(blocked, 0),
        "bounded synthetic requester exceeded its capacity");
    require(blocked.get() == blocked_raw && requester.size() == 3,
        "failed synthetic enqueue consumed ownership");
    require(!requester.canEnqueue(-1) && !requester.canEnqueue(2),
        "synthetic requester accepted an invalid VC");
    require(requester.recv(-1) == nullptr && requester.recv(2) == nullptr,
        "synthetic requester received from an invalid VC");

    std::unique_ptr<internal_router_event> received(requester.recv(0));
    require(received.get() == first_raw && requester.size() == 2,
        "synthetic requester did not dequeue the first VC head");
    require(requester.getVCHeads()[0] == second_raw,
        "synthetic requester did not advance its VC head");

    require(!requester.enqueue(blocked, -1) && blocked.get() == blocked_raw && requester.size() == 2,
        "invalid-VC enqueue consumed synthetic ownership");
    require(requester.enqueue(blocked, 1) && !blocked && requester.size() == 3,
        "synthetic requester did not accept work after capacity became available");
    require(requester.getVCHeads()[1] == third_raw,
        "enqueue behind a VC head changed FIFO order");

    received.reset(requester.recv(0));
    require(received.get() == second_raw && requester.getVCHeads()[0] == nullptr,
        "synthetic requester did not preserve VC 0 FIFO order");
    received.reset(requester.recv(1));
    require(received.get() == third_raw && requester.getVCHeads()[1] == blocked_raw,
        "synthetic requester did not preserve VC 1 FIFO order");
    received.reset(requester.recv(1));
    require(received.get() == blocked_raw && requester.size() == 0 && !requester.hasWork(),
        "synthetic requester did not drain to empty");
    require(requester.getVCHeads()[0] == nullptr && requester.getVCHeads()[1] == nullptr,
        "synthetic requester retained a stale head after drain");

    std::unique_ptr<internal_router_event> empty;
    require(!requester.enqueue(empty, 0) && requester.size() == 0,
        "synthetic requester accepted an empty event");
}

void
testSyntheticOutputQueues()
{
    NetworkServiceSyntheticRequester requester(1, 3);
    auto blocked_first = makeArbitrationEvent(0);
    auto blocked_second = makeArbitrationEvent(0);
    auto independent = makeArbitrationEvent(1);
    auto extra = makeArbitrationEvent(1);
    auto* first_raw = blocked_first.get();
    auto* second_raw = blocked_second.get();
    auto* independent_raw = independent.get();
    auto* extra_raw = extra.get();
    require(requester.enqueue(blocked_first, 0) && requester.enqueue(blocked_second, 0) &&
            requester.enqueue(independent, 0), "could not populate per-output synthetic queues");
    require(!requester.enqueue(extra, 0) && extra.get() == extra_raw && requester.size() == 3,
        "per-output queues exceeded aggregate capacity or consumed rejected ownership");

    FakeXbarPort output0;
    FakeXbarPort output1;
    PortInterface* outputs[2] = { &output0, &output1 };
    int output_busy[2] = { 1, 0 };
    requester.selectReadyHeads(outputs, output_busy);
    require(requester.getVCHeads()[0] == independent_raw,
        "a busy destination blocked a ready destination on the same VC");
    std::unique_ptr<internal_router_event> received(requester.recv(0));
    require(received.get() == independent_raw && requester.size() == 2,
        "output bypass removed the wrong packet");

    require(requester.enqueue(extra, 0) && !extra, "output bypass did not release aggregate capacity");
    output_busy[0] = 0;
    output0.setCredits(false);
    requester.selectReadyHeads(outputs, output_busy);
    require(requester.getVCHeads()[0] == extra_raw,
        "a destination without credits blocked an independent output");
    received.reset(requester.recv(0));
    require(received.get() == extra_raw, "credit bypass removed the wrong packet");

    requester.selectReadyHeads(outputs, output_busy);
    require(requester.getVCHeads()[0] == first_raw,
        "all-blocked output queues lost their head for stall accounting");
    output0.setCredits(true);
    requester.selectReadyHeads(outputs, output_busy);
    received.reset(requester.recv(0));
    require(received.get() == first_raw, "output bypass reordered packets to the same destination");
    requester.selectReadyHeads(outputs, output_busy);
    received.reset(requester.recv(0));
    require(received.get() == second_raw && !requester.hasWork(),
        "per-destination FIFO did not drain in order");
}

void
testExtendedRequestClone()
{
    ExtendedRequest original(7, 3, 64, true, true, new SST::Interfaces::StringEvent("payload"));
    original.giveServiceData(
        new PR2IntegrationServiceData(PR2IntegrationAction::Pass, 17));
    original.setMetadata("Reorder", ReorderMetadata(11));

    std::unique_ptr<ExtendedRequest> copy(original.clone());
    ReorderMetadata copied_metadata;
    const auto* copied_service = copy->inspectServiceDataAs<PR2IntegrationServiceData>();
    require(copy->inspectPayload() != nullptr && copy->inspectPayload() != original.inspectPayload(),
        "ExtendedRequest clone aliased its native payload");
    require(copied_service != nullptr && copied_service != original.inspectServiceData() &&
            copied_service->action() == PR2IntegrationAction::Pass && copied_service->sequence() == 17,
        "ExtendedRequest clone sliced or aliased its service data");
    require(copy->getMetadata("Reorder", copied_metadata) && copied_metadata.seq_number == 11,
        "ExtendedRequest clone lost plugin metadata");
}

void
testExtendedRequestCopyAndMoveOwnership()
{
    int clones = 0;
    int destructions = 0;
    {
        ExtendedRequest original(7, 3, 64, true, true,
            new CloneTrackingEvent(clones, destructions));
        original.setMetadata("Reorder", ReorderMetadata(19));
        SST::Event* original_payload = original.inspectPayload();

        ExtendedRequest copy(original);
        require(clones == 1 && copy.inspectPayload() != nullptr &&
                copy.inspectPayload() != original_payload && original.inspectPayload() == original_payload,
            "ExtendedRequest copy did not deep-clone its native payload");

        ExtendedRequest assigned(1, 1, 8, true, true,
            new CloneTrackingEvent(clones, destructions));
        assigned = original;
        require(clones == 2 && destructions == 1 && assigned.inspectPayload() != original_payload,
            "ExtendedRequest copy assignment leaked or aliased its native payload");

        SST::Event* moved_payload = copy.inspectPayload();
        const int clones_before_move = clones;
        ExtendedRequest moved(std::move(copy));
        require(clones == clones_before_move && moved.inspectPayload() == moved_payload &&
                copy.inspectPayload() == nullptr,
            "ExtendedRequest move construction cloned or aliased ownership");

        SST::Event* move_assigned_payload = assigned.inspectPayload();
        ExtendedRequest move_assigned;
        move_assigned = std::move(assigned);
        require(clones == clones_before_move && move_assigned.inspectPayload() == move_assigned_payload &&
                assigned.inspectPayload() == nullptr,
            "ExtendedRequest move assignment cloned or aliased ownership");
    }
    require(destructions == 4, "ExtendedRequest copy/move payloads were not destroyed exactly once");
}

void
testExtendedRequestLegacyPointerWrapper()
{
    static_assert(std::is_convertible_v<SST::Interfaces::SimpleNetwork::Request*, ExtendedRequest>,
        "released Request-pointer conversion became explicit");

    int clones = 0;
    int destructions = 0;
    auto* request = new SST::Interfaces::SimpleNetwork::Request(
        7, 3, 64, true, true, new CloneTrackingEvent(clones, destructions));
    request->vn = 9;
    request->allow_adaptive = false;
    request->setTraceType(SST::Interfaces::SimpleNetwork::Request::FULL);
    request->setTraceID(23);
    SST::Event* payload = request->inspectPayload();

    ExtendedRequest wrapped = request;
    require(wrapped.dest == 7 && wrapped.src == 3 && wrapped.size_in_bits == 64 &&
            wrapped.head && wrapped.tail && wrapped.vn == 0 && wrapped.allow_adaptive &&
            wrapped.getTraceType() == SST::Interfaces::SimpleNetwork::Request::FULL &&
            wrapped.getTraceID() == 23 && wrapped.inspectPayload() == payload &&
            request->inspectPayload() == nullptr && clones == 0,
        "ExtendedRequest pointer wrapper changed released ordinary-request behavior");
    delete request;
    require(destructions == 0, "ExtendedRequest pointer wrapper did not transfer payload ownership");
}

void
testExtendedRequestCopyExceptionSafety()
{
    for ( bool throw_on_clone : { false, true } ) {
        int clones = 0;
        int destructions = 0;
        {
            ExtendedRequest original(7, 3, 64, true, true,
                new CloneTrackingEvent(clones, destructions, !throw_on_clone, throw_on_clone));
            SST::Event* original_payload = original.inspectPayload();
            bool threw = false;
            try {
                ExtendedRequest copy(original);
            }
            catch ( const std::runtime_error& ) {
                threw = true;
            }
            require(threw && clones == 1 && destructions == 0 &&
                    original.inspectPayload() == original_payload,
                "failed ExtendedRequest copy damaged source payload ownership");
        }
        require(destructions == 1,
            "failed ExtendedRequest copy did not leave source payload singly owned");
    }
}

void
testLegacyRouterEventAndEnvelopeOwnership()
{
    int clones = 0;
    int destructions = 0;
    {
        auto* request = new SST::Interfaces::SimpleNetwork::Request(
            4, 2, 65, true, true, new CloneTrackingEvent(clones, destructions));
        RtrEvent envelope(request, 2, 0);
        require(envelope.computeSizeInFlits(64) && envelope.getSizeInFlits() == 2 &&
                envelope.hasValidTransportMetadata(),
            "legacy computeSizeInFlits did not establish valid transport metadata");

        std::unique_ptr<RtrEvent> clone(envelope.clone());
        std::unique_ptr<SST::Interfaces::SimpleNetwork::Request> cloned_request(clone->takeRequest());
        require(clones == 1 && cloned_request && cloned_request->inspectPayload() != nullptr &&
                cloned_request->inspectPayload() != request->inspectPayload(),
            "RtrEvent clone aliased request payload ownership");

        RtrEvent assigned(new SST::Interfaces::SimpleNetwork::Request(
            1, 0, 8, true, true, new CloneTrackingEvent(clones, destructions)), 0, 0);
        assigned = envelope;
        std::unique_ptr<SST::Interfaces::SimpleNetwork::Request> assigned_request(assigned.takeRequest());
        require(clones == 2 && destructions == 1 && assigned_request.get() != request &&
                assigned_request->inspectPayload() != request->inspectPayload(),
            "RtrEvent copy assignment leaked or aliased request ownership");
    }
    require(destructions == 4, "RtrEvent copied payloads were not destroyed exactly once");

    int internal_clones = 0;
    int internal_destructions = 0;
    {
        internal_router_event original(new RtrEvent(new SST::Interfaces::SimpleNetwork::Request(
            4, 2, 64, true, true,
            new CloneTrackingEvent(internal_clones, internal_destructions)), 2, 0));
        original.setNextPort(3);
        original.setVC(1);
        original.setCreditReturnVC(2);
        internal_router_event assigned(new RtrEvent(new SST::Interfaces::SimpleNetwork::Request(
            1, 0, 8, true, true,
            new CloneTrackingEvent(internal_clones, internal_destructions)), 0, 0));
        assigned = original;
        require(internal_clones == 1 && internal_destructions == 1 &&
                assigned.getEncapsulatedEvent() != original.getEncapsulatedEvent() &&
                assigned.getNextPort() == 3 && assigned.getVC() == 1 &&
                assigned.getCreditReturnVC() == 2,
            "internal_router_event copy assignment sliced or aliased owned state");
    }
    require(internal_destructions == 3,
        "internal_router_event copy assignment did not destroy payloads exactly once");

    int replacement_clones = 0;
    int replacement_destructions = 0;
    auto* first = new RtrEvent(new SST::Interfaces::SimpleNetwork::Request(
        1, 0, 8, true, true, new CloneTrackingEvent(replacement_clones, replacement_destructions)), 0, 0);
    auto* second = new RtrEvent(new SST::Interfaces::SimpleNetwork::Request(
        1, 0, 8, true, true, new CloneTrackingEvent(replacement_clones, replacement_destructions)), 0, 0);
    internal_router_event wrapper(first);
    wrapper.setEncapsulatedEvent(nullptr);
    require(replacement_destructions == 0,
        "setEncapsulatedEvent changed its legacy non-destroying assignment contract");
    delete first;
    wrapper.setEncapsulatedEvent(second);
    std::unique_ptr<RtrEvent> transferred(wrapper.takeEncapsulatedEvent());
    require(transferred.get() == second && replacement_destructions == 1,
        "takeEncapsulatedEvent did not transfer without deletion");
    transferred.reset();
    require(replacement_destructions == 2,
        "transferred encapsulated event was not singly owned");
}

void
testRoundRobinRejectsActiveService()
{
    xbar_arb_rr arbiter;
    XbarArbitration& api = arbiter;
    require(!api.setNetworkServiceInputs(3, 2, 2, { 1, 0 }),
        "RR accepted processor-owned VCs");
    require(api.setNetworkServiceInputs(3, 2, 2, { 0, 0 }),
        "RR rejected a transparent pass processor");
}

void
testOwnedVCsAreNeverArbitratedForPhysicalInputs()
{
    // Two VCs; the processor owns VC 0.  A physical head on VC 0 must never
    // be granted, a physical head on VC 1 must, and the synthetic input may
    // use VC 0 because it carries processor output.
    auto owned_event = makeArbitrationEvent(1);
    auto free_event = makeArbitrationEvent(1);
    auto synthetic_event = makeArbitrationEvent(0);
    FakeXbarPort2 port0;
    FakeXbarPort2 port1;
    FakeXbarPort2 synthetic;
    port0.setHead(0, owned_event.get());
    port1.setHead(1, free_event.get());
    synthetic.setHead(0, synthetic_event.get());
    NetworkServicePortXbarInput input0(&port0);
    NetworkServicePortXbarInput input1(&port1);
    NetworkServicePortXbarInput synthetic_input(&synthetic);
    XbarInput* inputs[3] = { &input0, &input1, &synthetic_input };
    PortInterface* outputs[2] = { &port0, &port1 };
    xbar_arb_lru arbiter;
    XbarArbitration& arbiter_api = arbiter;
    require(!arbiter_api.setNetworkServiceInputs(3, 2, 2, std::vector<uint8_t>(1, 0)),
        "owned-VC mask with the wrong length was accepted");
    require(arbiter_api.setNetworkServiceInputs(3, 2, 2, std::vector<uint8_t> { 1, 0 }),
        "service LRU rejected an owned-VC mask");

    int owned_grants = 0;
    int free_grants = 0;
    int synthetic_grants = 0;
    for ( int cycle = 0; cycle < 12; ++cycle ) {
        int input_busy[3] = { 0, 0, 0 };
        int output_busy[2] = { 0, 0 };
        int progress[3] = { -1, -1, -1 };
        require(arbiter_api.arbitrateNetworkService(inputs, outputs, input_busy, output_busy, progress),
            "owned-VC arbitration failed");
        require(progress[0] != -2, "an owned VC head counted as a stalled physical input");
        owned_grants += progress[0] >= 0;
        free_grants += progress[1] == 1;
        synthetic_grants += progress[2] == 0;
    }
    require(owned_grants == 0, "a physical head on a processor-owned VC was granted");
    require(free_grants == 12, "a physical head on an unowned VC was not granted every cycle");
    require(synthetic_grants > 0, "the synthetic input was denied a processor-owned VC");
}

void
testIndependentOutputProgress()
{
    // A synthetic head on output 0 must not freeze fair service between
    // ordinary inputs competing for the independent, always-ready output 1.
    // Exercise both forms of backpressure and an always-ready synthetic head.
    for ( int scenario = 0; scenario < 3; ++scenario ) {
        auto ordinary0 = makeArbitrationEvent(1);
        auto ordinary1 = makeArbitrationEvent(1);
        auto service = makeArbitrationEvent(0);
        FakeXbarPort port0;
        FakeXbarPort port1;
        FakeXbarPort synthetic;
        port0.setHead(ordinary0.get());
        port1.setHead(ordinary1.get());
        synthetic.setHead(service.get());
        port0.setCredits(scenario != 1);
        NetworkServicePortXbarInput input0(&port0);
        NetworkServicePortXbarInput input1(&port1);
        NetworkServicePortXbarInput service_input(&synthetic);
        XbarInput* inputs[3] = { &input0, &input1, &service_input };
        PortInterface* outputs[2] = { &port0, &port1 };
        xbar_arb_lru arbiter;
        XbarArbitration& api = arbiter;
        require(api.setNetworkServiceInputs(3, 2, 1, std::vector<uint8_t>(1, 0)),
            "independent-output LRU rejected a valid input split");

        int grants[2] = { 0, 0 };
        int last_grant[2] = { -1, -1 };
        for ( int cycle = 0; cycle < 24; ++cycle ) {
            int input_busy[3] = { 0, 0, 0 };
            int output_busy[2] = { scenario == 0 ? 1 : 0, 0 };
            int progress[3] = { -1, -1, -1 };
            require(api.arbitrateNetworkService(inputs, outputs, input_busy, output_busy, progress),
                "independent-output LRU arbitration failed");
            require((progress[0] >= 0) != (progress[1] >= 0),
                "independent ordinary output was idle or granted twice");
            require((progress[2] >= 0) == (scenario == 2),
                "synthetic grant ignored output availability");
            for ( int input = 0; input < 2; ++input ) {
                if ( progress[input] >= 0 ) {
                    ++grants[input];
                    last_grant[input] = cycle;
                }
                require(cycle - last_grant[input] <= 3,
                    "an unrelated synthetic head starved a continuously eligible ordinary input");
            }
        }
        require(grants[0] == 12 && grants[1] == 12,
            "synthetic traffic unbalanced independent equal-load ordinary flows");
    }
}

void
testMixedVCDestinations()
{
    // One physical input alternates traffic between an independent output
    // and an output contested by the synthetic input. Both VCs must progress.
    auto independent = makeArbitrationEvent(1);
    auto contested = makeArbitrationEvent(0);
    auto service = makeArbitrationEvent(0);
    contested->setVC(1);
    FakeXbarPort2 port0;
    FakeXbarPort2 port1;
    FakeXbarPort2 synthetic;
    port0.setHead(0, independent.get());
    port0.setHead(1, contested.get());
    synthetic.setHead(0, service.get());
    NetworkServicePortXbarInput input0(&port0);
    NetworkServicePortXbarInput input1(&port1);
    NetworkServicePortXbarInput service_input(&synthetic);
    XbarInput* inputs[3] = { &input0, &input1, &service_input };
    PortInterface* outputs[2] = { &port0, &port1 };
    xbar_arb_lru arbiter;
    XbarArbitration& api = arbiter;
    require(api.setNetworkServiceInputs(3, 2, 2, std::vector<uint8_t>(2, 0)),
        "mixed-destination LRU rejected a valid input split");

    int last_grant[3] = { -1, -1, -1 };
    for ( int cycle = 0; cycle < 24; ++cycle ) {
        int input_busy[3] = { 0, 0, 0 };
        int output_busy[2] = { 0, 0 };
        int progress[3] = { -1, -1, -1 };
        require(api.arbitrateNetworkService(inputs, outputs, input_busy, output_busy, progress),
            "mixed-destination LRU arbitration failed");
        require(progress[0] == 0 || progress[0] == 1,
            "physical input with two ready destinations made no progress");
        last_grant[progress[0]] = cycle;
        if ( progress[2] >= 0 ) last_grant[2] = cycle;
        require(progress[0] != 1 || progress[2] < 0,
            "ordinary and synthetic packets were granted the same output together");
        for ( int flow = 0; flow < 3; ++flow ) {
            require(cycle - last_grant[flow] <= 3,
                "mixed destinations starved an eligible ordinary VC or synthetic input");
        }
    }
}

void
testSyntheticLRUFairness()
{
    auto event0 = makeArbitrationEvent();
    auto event1 = makeArbitrationEvent();
    auto event2 = makeArbitrationEvent();
    FakeXbarPort port0;
    FakeXbarPort port1;
    FakeXbarPort synthetic;
    port0.setHead(event0.get());
    port1.setHead(event1.get());
    synthetic.setHead(event2.get());
    NetworkServicePortXbarInput input0(&port0);
    NetworkServicePortXbarInput input1(&port1);
    NetworkServicePortXbarInput synthetic_input(&synthetic);
    XbarInput* inputs[3] = { &input0, &input1, &synthetic_input };
    PortInterface* outputs[2] = { &port0, &port1 };
    xbar_arb_lru arbiter;
    XbarArbitration& arbiter_api = arbiter;
    require(arbiter_api.setNetworkServiceInputs(3, 2, 1, { 0 }),
        "service LRU rejected a valid input split");
    int grants[3] = { 0, 0, 0 };
    int last_grant[3] = { -1, -1, -1 };
    for ( int cycle = 0; cycle < 90; ++cycle ) {
        int input_busy[3] = { 0, 0, 0 };
        int output_busy[2] = { 0, 0 };
        int progress[3] = { -1, -1, -1 };
        require(arbiter_api.arbitrateNetworkService(inputs, outputs, input_busy, output_busy, progress),
            "service LRU arbitration failed");
        int winners = 0;
        for ( int input = 0; input < 3; ++input ) {
            if ( progress[input] >= 0 ) {
                ++winners;
                ++grants[input];
                last_grant[input] = cycle;
            }
            require(cycle - last_grant[input] <= 3,
                "service LRU starved an eligible physical or synthetic input");
        }
        require(winners == 1, "same-output service LRU granted multiple inputs");
    }
    require(grants[0] == 30 && grants[1] == 30 && grants[2] == 30,
        "service LRU did not share a contended output fairly");
}

void
testDormantSyntheticLRUMatchesOrdinary()
{
    auto event00 = makeArbitrationEvent(1);
    auto event01 = makeArbitrationEvent(0);
    auto event10 = makeArbitrationEvent(0);
    auto event11 = makeArbitrationEvent(1);
    event01->setVC(1);
    event11->setVC(1);
    FakeXbarPort2 port0;
    FakeXbarPort2 port1;
    FakeXbarPort2 synthetic;
    NetworkServicePortXbarInput input0(&port0);
    NetworkServicePortXbarInput input1(&port1);
    NetworkServicePortXbarInput synthetic_input(&synthetic);
    XbarInput* inputs[3] = { &input0, &input1, &synthetic_input };
    PortInterface* ports[2] = { &port0, &port1 };
    xbar_arb_lru ordinary;
    xbar_arb_lru service;
    XbarArbitration& ordinary_api = ordinary;
    XbarArbitration& service_api = service;
    ordinary_api.setPorts(2, 2);
    require(service_api.setNetworkServiceInputs(3, 2, 2, { 0, 0 }),
        "dormant-synthetic LRU rejected a valid input split");
    for ( int cycle = 0; cycle < 100; ++cycle ) {
        port0.setHead(0, cycle % 7 ? event00.get() : nullptr);
        port0.setHead(1, cycle % 3 ? event01.get() : nullptr);
        port1.setHead(0, cycle % 4 ? event10.get() : nullptr);
        port1.setHead(1, cycle % 5 ? event11.get() : nullptr);
        int ordinary_input_busy[2] = { cycle % 11 == 0, cycle % 13 == 0 };
        int ordinary_output_busy[2] = { cycle % 3 == 0, cycle % 7 == 0 };
        int service_input_busy[3] = { ordinary_input_busy[0], ordinary_input_busy[1], 0 };
        int service_output_busy[2] = { ordinary_output_busy[0], ordinary_output_busy[1] };
        int ordinary_progress[2] = { -1, -1 };
        int service_progress[3] = { -1, -1, -1 };
        ordinary_api.arbitrate(ports, ordinary_input_busy, ordinary_output_busy, ordinary_progress);
        require(service_api.arbitrateNetworkService(inputs, ports, service_input_busy,
                    service_output_busy, service_progress), "dormant-synthetic LRU arbitration failed");
        for ( int port = 0; port < 2; ++port ) {
            require(ordinary_progress[port] == service_progress[port] &&
                    ordinary_input_busy[port] == service_input_busy[port] &&
                    ordinary_output_busy[port] == service_output_busy[port],
                "dormant synthetic input changed physical LRU grants or transfer timing");
        }
        require(service_progress[2] == -1 && service_input_busy[2] == 0,
            "dormant synthetic input received a crossbar grant");
    }
}

} // namespace

NetworkServiceTest::NetworkServiceTest(SST::ComponentId_t id, SST::Params& params) : SST::Component(id)
{
    (void)params;
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();

    SST::Output output("", 1, 0, SST::Output::STDOUT);
    try {
        testDispositions();
        testBoundedSyntheticRequester();
        testSyntheticOutputQueues();
        testExtendedRequestClone();
        testExtendedRequestCopyAndMoveOwnership();
        testExtendedRequestLegacyPointerWrapper();
        testExtendedRequestCopyExceptionSafety();
        testLegacyRouterEventAndEnvelopeOwnership();
        testRoundRobinRejectsActiveService();
        testOwnedVCsAreNeverArbitratedForPhysicalInputs();
        testIndependentOutputProgress();
        testMixedVCDestinations();
        testSyntheticLRUFairness();
        testDormantSyntheticLRUMatchesOrdinary();
    }
    catch ( const std::exception& error ) {
        output.fatal(CALL_INFO, -1, "Merlin network-service acceptance contract FAIL: %s\n", error.what());
    }

    output.output("Merlin network-service acceptance contract PASS\n");
    primaryComponentOKToEndSim();
}

} // namespace SST::Merlin
