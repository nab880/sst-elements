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
#include "sst/elements/merlin/networkService.h"

#include <sst/core/output.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
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
    NetworkServiceHeadIdentity inspectNetworkServiceHead(int) const override { return {}; }
    internal_router_event* recvNetworkServiceExpected(
        int, const NetworkServiceHeadIdentity&) override { return nullptr; }
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
makeArbitrationEvent()
{
    auto* request = new SST::Interfaces::SimpleNetwork::Request(0, 0, 64, true, true);
    request->vn = 0;
    auto* envelope = new RtrEvent(request, 0, 0);
    require(envelope->setSyntheticTransportMetadata(1, 0), "could not create arbitration envelope");
    auto event = std::make_unique<internal_router_event>(envelope);
    event->setNextPort(0);
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

    XbarInput* inputs[3] = { &port0, &port1, &synthetic };
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
        testSyntheticRoundRobinFairness();
    }
    catch ( const std::exception& error ) {
        output.fatal(CALL_INFO, -1, "Merlin network-service transaction contract FAIL: %s\n", error.what());
    }

    output.output("Merlin network-service transaction contract PASS\n");
    primaryComponentOKToEndSim();
}

} // namespace SST::Merlin
