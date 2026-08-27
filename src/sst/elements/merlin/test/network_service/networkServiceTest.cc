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

constexpr int kTestVC = 0;

void
require(bool condition, const char* message)
{
    if ( !condition ) throw std::runtime_error(message);
}

class TrackedRouterEvent final : public internal_router_event
{
public:
    explicit TrackedRouterEvent(int& destructions) : destructions_(&destructions) {}

    ~TrackedRouterEvent() override { ++*destructions_; }

private:
    int* destructions_;
};

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

class FakeIngressQueue final : public NetworkServiceIngressQueue
{
public:
    explicit FakeIngressQueue(std::unique_ptr<internal_router_event> event) : event_(std::move(event)) {}

    NetworkServiceHeadIdentity inspectNetworkServiceHead(int vc) const override
    {
        if ( vc != kTestVC || !event_ ) return {};
        return { event_.get(), generation_ };
    }

    internal_router_event* recvNetworkServiceExpected(
        int vc, const NetworkServiceHeadIdentity& expected) override
    {
        ++recv_calls_;
        const NetworkServiceHeadIdentity actual = inspectNetworkServiceHead(vc);
        if ( !expected.valid() || actual.event != expected.event || actual.generation != expected.generation ) {
            return nullptr;
        }
        advanceGeneration();
        return event_.release();
    }

    void makeExpectedStale() { advanceGeneration(); }

    bool owns(const internal_router_event* event) const { return event_.get() == event; }
    bool empty() const { return !event_; }
    int recvCalls() const { return recv_calls_; }

private:
    void advanceGeneration()
    {
        if ( ++generation_ == 0 ) ++generation_;
    }

    std::unique_ptr<internal_router_event> event_;
    uint64_t generation_ = 1;
    int recv_calls_ = 0;
};

struct ReservationState
{
    int commits = 0;
    int rollbacks = 0;
    std::unique_ptr<internal_router_event> committed_event;
};

class TrackingReservation final : public NetworkServiceReservation
{
public:
    explicit TrackingReservation(std::shared_ptr<ReservationState> state) : state_(std::move(state)) {}

    void commit(std::unique_ptr<internal_router_event> event) noexcept override
    {
        ++state_->commits;
        state_->committed_event = std::move(event);
    }

    void rollback() noexcept override { ++state_->rollbacks; }

private:
    std::shared_ptr<ReservationState> state_;
};

std::unique_ptr<internal_router_event>
makeTrackedEvent(int& destructions)
{
    return std::make_unique<TrackedRouterEvent>(destructions);
}

class FakeXbarPort final : public PortInterface
{
public:
    void setHead(internal_router_event* event) { heads_[0] = event; }

    void recvCtrlEvent(CtrlRtrEvent*) override {}
    void sendCtrlEvent(CtrlRtrEvent*) override {}
    void send(internal_router_event*, int) override {}
    bool spaceToSend(int vc, int flits) override { return vc == 0 && flits == 1; }
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
testPassAndBusy()
{
    int destructions = 0;
    FakeIngressQueue queue(makeTrackedEvent(destructions));
    const NetworkServiceHeadIdentity expected = queue.inspectNetworkServiceHead(kTestVC);

    uint64_t diagnostic = 0;
    NetworkServicePrepared pass(NetworkServiceDisposition::Pass, {}, 0x11);
    require(applyNetworkServicePrepared(queue, kTestVC, expected, std::move(pass), diagnostic) ==
            NetworkServiceApplyResult::Passed,
        "PASS returned the wrong apply result");
    require(diagnostic == 0x11, "PASS did not preserve its opaque diagnostic");
    require(queue.recvCalls() == 0 && queue.owns(expected.event) && destructions == 0,
        "PASS changed queue ownership");

    NetworkServicePrepared busy(NetworkServiceDisposition::Busy, {}, 0x22);
    require(applyNetworkServicePrepared(queue, kTestVC, expected, std::move(busy), diagnostic) ==
            NetworkServiceApplyResult::Busy,
        "BUSY returned the wrong apply result");
    require(diagnostic == 0x22, "BUSY did not preserve its opaque diagnostic");
    require(queue.recvCalls() == 0 && queue.owns(expected.event) && destructions == 0,
        "BUSY changed queue ownership");
}

void
testAccept()
{
    int destructions = 0;
    FakeIngressQueue queue(makeTrackedEvent(destructions));
    const NetworkServiceHeadIdentity expected = queue.inspectNetworkServiceHead(kTestVC);
    auto state = std::make_shared<ReservationState>();

    uint64_t diagnostic = 0;
    NetworkServicePrepared accept(NetworkServiceDisposition::Accept,
        std::make_unique<TrackingReservation>(state), 0x33);
    require(applyNetworkServicePrepared(queue, kTestVC, expected, std::move(accept), diagnostic) ==
            NetworkServiceApplyResult::Accepted,
        "ACCEPT returned the wrong apply result");
    require(diagnostic == 0x33, "ACCEPT did not preserve its opaque diagnostic");
    require(queue.recvCalls() == 1 && queue.empty(), "ACCEPT did not dequeue exactly once");
    require(state->commits == 1 && state->rollbacks == 0,
        "ACCEPT did not commit its reservation exactly once");
    require(state->committed_event.get() == expected.event && destructions == 0,
        "ACCEPT did not transfer the exact head to its reservation");

    state->committed_event.reset();
    require(destructions == 1, "accepted event was not owned by the committed reservation");
}

void
testStaleAcceptRollsBack()
{
    int destructions = 0;
    FakeIngressQueue queue(makeTrackedEvent(destructions));
    const NetworkServiceHeadIdentity stale = queue.inspectNetworkServiceHead(kTestVC);
    queue.makeExpectedStale();
    auto state = std::make_shared<ReservationState>();

    uint64_t diagnostic = 0;
    NetworkServicePrepared accept(NetworkServiceDisposition::Accept,
        std::make_unique<TrackingReservation>(state), 0x44);
    require(applyNetworkServicePrepared(queue, kTestVC, stale, std::move(accept), diagnostic) ==
            NetworkServiceApplyResult::HeadChanged,
        "stale ACCEPT did not report a changed head");
    require(diagnostic == 0x44, "stale ACCEPT did not preserve its opaque diagnostic");
    require(queue.recvCalls() == 1 && queue.owns(stale.event) && destructions == 0,
        "stale ACCEPT changed queue ownership");
    require(state->commits == 0 && state->rollbacks == 1 && !state->committed_event,
        "stale ACCEPT did not roll back exactly once");
}

void
testReject()
{
    int destructions = 0;
    FakeIngressQueue queue(makeTrackedEvent(destructions));
    const NetworkServiceHeadIdentity expected = queue.inspectNetworkServiceHead(kTestVC);

    uint64_t diagnostic = 0;
    NetworkServicePrepared reject(NetworkServiceDisposition::Reject, {}, 0x55);
    require(applyNetworkServicePrepared(queue, kTestVC, expected, std::move(reject), diagnostic) ==
            NetworkServiceApplyResult::Rejected,
        "REJECT returned the wrong apply result");
    require(diagnostic == 0x55, "REJECT did not preserve its opaque diagnostic");
    require(queue.recvCalls() == 1 && queue.empty(), "REJECT did not dequeue exactly once");
    require(destructions == 1, "REJECT did not destroy the dequeued event exactly once");
}

void
testInvalidReservations()
{
    {
        int destructions = 0;
        FakeIngressQueue queue(makeTrackedEvent(destructions));
        const NetworkServiceHeadIdentity expected = queue.inspectNetworkServiceHead(kTestVC);
        auto state = std::make_shared<ReservationState>();

        uint64_t diagnostic = 0;
        NetworkServicePrepared invalid(NetworkServiceDisposition::Pass,
            std::make_unique<TrackingReservation>(state), 0x66);
        require(applyNetworkServicePrepared(queue, kTestVC, expected, std::move(invalid), diagnostic) ==
                NetworkServiceApplyResult::InvalidPrepared,
            "reservation attached to PASS was accepted");
        require(diagnostic == 0x66, "invalid reservation did not preserve its opaque diagnostic");
        require(queue.recvCalls() == 0 && queue.owns(expected.event) && destructions == 0,
            "invalid reservation changed queue ownership");
        require(state->commits == 0 && state->rollbacks == 1,
            "invalid reservation was not rolled back exactly once");
    }

    {
        int destructions = 0;
        FakeIngressQueue queue(makeTrackedEvent(destructions));
        const NetworkServiceHeadIdentity expected = queue.inspectNetworkServiceHead(kTestVC);

        uint64_t diagnostic = 0;
        NetworkServicePrepared missing(NetworkServiceDisposition::Accept, {}, 0x77);
        require(applyNetworkServicePrepared(queue, kTestVC, expected, std::move(missing), diagnostic) ==
                NetworkServiceApplyResult::InvalidPrepared,
            "ACCEPT without a reservation was accepted");
        require(diagnostic == 0x77, "missing reservation did not preserve its opaque diagnostic");
        require(queue.recvCalls() == 0 && queue.owns(expected.event) && destructions == 0,
            "missing reservation changed queue ownership");
    }
}

void
testBoundedSyntheticRequester()
{
    NetworkServiceSyntheticRequester requester(2, 3);

    auto first = std::make_unique<internal_router_event>();
    auto second = std::make_unique<internal_router_event>();
    auto third = std::make_unique<internal_router_event>();
    auto blocked = std::make_unique<internal_router_event>();
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
testPortInterfaceCompatibilityDefaults()
{
    FakeXbarPort legacy_port;
    require(!legacy_port.inspectNetworkServiceHead(0).valid() &&
            legacy_port.recvNetworkServiceExpected(0, {}) == nullptr,
        "PortInterface disabled service defaults changed legacy subclasses");
}

void
testSyntheticRoundRobinFairness()
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
    xbar_arb_rr arbiter;
    XbarArbitration& arbiter_api = arbiter;
    require(arbiter_api.setNetworkServiceInputs(3, 2, 1), "service RR rejected a valid input split");

    int grants[3] = { 0, 0, 0 };
    for ( int cycle = 0; cycle < 90; ++cycle ) {
        int input_busy[3] = { 0, 0, 0 };
        int output_busy[2] = { 0, 0 };
        int progress[3] = { -1, -1, -1 };
        require(arbiter_api.arbitrateNetworkService(inputs, outputs, input_busy, output_busy, progress),
            "service RR arbitration failed");
        int winners = 0;
        int winner = -1;
        for ( int input = 0; input < 3; ++input ) {
            if ( progress[input] >= 0 ) {
                ++grants[input];
                ++winners;
                winner = input;
            }
        }
        require(winners == 1, "same-output service RR granted the wrong number of inputs");
        require(winner == cycle % 3, "service RR did not provide bounded per-input rotation");
    }
    require(grants[0] == 30 && grants[1] == 30 && grants[2] == 30,
        "synthetic requester did not receive equal per-input RR service");

    xbar_arb_rr blocked_arbiter;
    XbarArbitration& blocked_api = blocked_arbiter;
    require(blocked_api.setNetworkServiceInputs(3, 2, 1),
        "blocked-cycle RR rejected a valid input split");
    {
        int input_busy[3] = { 0, 0, 0 };
        int output_busy[2] = { 1, 0 };
        int progress[3] = { -1, -1, -1 };
        require(blocked_api.arbitrateNetworkService(
                    inputs, outputs, input_busy, output_busy, progress),
            "blocked-cycle service RR arbitration failed");
        require(progress[0] < 0 && progress[1] < 0 && progress[2] < 0,
            "blocked-cycle service RR unexpectedly granted an input");
    }
    {
        int input_busy[3] = { 0, 0, 0 };
        int output_busy[2] = { 0, 0 };
        int progress[3] = { -1, -1, -1 };
        require(blocked_api.arbitrateNetworkService(
                    inputs, outputs, input_busy, output_busy, progress),
            "post-block service RR arbitration failed");
        require(progress[0] == 0 && progress[1] < 0 && progress[2] < 0,
            "a blocked cycle consumed the service RR input's turn");
    }

    auto unrelated_event = makeArbitrationEvent(1);
    auto contender_event = makeArbitrationEvent(0);
    auto periodic_synthetic_event = makeArbitrationEvent(0);
    FakeXbarPort unrelated_port;
    FakeXbarPort contender_port;
    FakeXbarPort periodic_synthetic;
    unrelated_port.setHead(unrelated_event.get());
    contender_port.setHead(contender_event.get());
    periodic_synthetic.setHead(periodic_synthetic_event.get());
    NetworkServicePortXbarInput unrelated_input(&unrelated_port);
    NetworkServicePortXbarInput contender_input(&contender_port);
    NetworkServicePortXbarInput periodic_synthetic_input(&periodic_synthetic);
    XbarInput* periodic_inputs[3] = { &unrelated_input, &contender_input, &periodic_synthetic_input };
    PortInterface* periodic_outputs[2] = { &contender_port, &unrelated_port };
    xbar_arb_rr periodic_arbiter;
    XbarArbitration& periodic_api = periodic_arbiter;
    require(periodic_api.setNetworkServiceInputs(3, 2, 1),
        "periodic-block RR rejected a valid input split");
    int synthetic_grants = 0;
    for ( int cycle = 0; cycle < 8; ++cycle ) {
        int input_busy[3] = { 0, 0, 0 };
        int output_busy[2] = { cycle % 2 == 0 ? 1 : 0, 0 };
        int progress[3] = { -1, -1, -1 };
        require(periodic_api.arbitrateNetworkService(
                    periodic_inputs, periodic_outputs, input_busy, output_busy, progress),
            "periodic-block service RR arbitration failed");
        require(progress[0] == 0, "unrelated continuously available output did not grant");
        if ( progress[2] == 0 ) ++synthetic_grants;
    }
    require(synthetic_grants > 0,
        "unrelated grants phase-locked a synthetic head against periodic credits");

    xbar_arb_rr service_empty;
    xbar_arb_rr ordinary;
    XbarArbitration& service_empty_api = service_empty;
    XbarArbitration& ordinary_api = ordinary;
    require(service_empty_api.setNetworkServiceInputs(3, 2, 1),
        "empty-synthetic RR rejected a valid input split");
    ordinary_api.setPorts(2, 1);
    PortInterface* physical_ports[2] = { &port0, &port1 };

    for ( int cycle = 0; cycle < 7; ++cycle ) {
        int service_input_busy[3] = { 0, 0, 0 };
        int service_output_busy[2] = { 0, 0 };
        int service_progress[3] = { -1, -1, -1 };
        int ordinary_input_busy[2] = { 0, 0 };
        int ordinary_output_busy[2] = { 0, 0 };
        int ordinary_progress[2] = { -1, -1 };
        require(service_empty_api.arbitrateNetworkService(inputs, outputs, service_input_busy,
                    service_output_busy, service_progress),
            "active-synthetic handoff setup failed");
        ordinary_api.arbitrate(physical_ports, ordinary_input_busy, ordinary_output_busy, ordinary_progress);
    }
    service_empty_api.reportSkippedCycles(5);
    ordinary_api.reportSkippedCycles(5);
    synthetic.setHead(nullptr);

    for ( int cycle = 0; cycle < 24; ++cycle ) {
        int service_input_busy[3] = { 0, 0, 0 };
        int service_output_busy[2] = { 0, 0 };
        int service_progress[3] = { -1, -1, -1 };
        int ordinary_input_busy[2] = { 0, 0 };
        int ordinary_output_busy[2] = { 0, 0 };
        int ordinary_progress[2] = { -1, -1 };
        require(service_empty_api.arbitrateNetworkService(inputs, outputs, service_input_busy,
                    service_output_busy, service_progress),
            "empty-synthetic service RR arbitration failed");
        ordinary_api.arbitrate(physical_ports, ordinary_input_busy, ordinary_output_busy, ordinary_progress);
        const bool same_physical_grants =
            ((service_progress[0] >= 0) == (ordinary_progress[0] >= 0)) &&
            ((service_progress[1] >= 0) == (ordinary_progress[1] >= 0)) &&
            (service_progress[0] < 0 || service_progress[0] == ordinary_progress[0]) &&
            (service_progress[1] < 0 || service_progress[1] == ordinary_progress[1]);
        require(same_physical_grants && service_progress[2] == -1,
            "empty synthetic input changed physical RR ordering");
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
        testPassAndBusy();
        testAccept();
        testStaleAcceptRollsBack();
        testReject();
        testInvalidReservations();
        testBoundedSyntheticRequester();
        testExtendedRequestClone();
        testExtendedRequestCopyAndMoveOwnership();
        testExtendedRequestLegacyPointerWrapper();
        testExtendedRequestCopyExceptionSafety();
        testLegacyRouterEventAndEnvelopeOwnership();
        testPortInterfaceCompatibilityDefaults();
        testSyntheticRoundRobinFairness();
    }
    catch ( const std::exception& error ) {
        output.fatal(CALL_INFO, -1, "Merlin network-service transaction contract FAIL: %s\n", error.what());
    }

    output.output("Merlin network-service transaction contract PASS\n");
    primaryComponentOKToEndSim();
}

} // namespace SST::Merlin
