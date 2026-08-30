// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "sst_config.h"

#include <sst/elements/merlin/services/collective/merlinStaticCollectiveProcessor.h>

#include <sst/core/component.h>
#include <sst/core/output.h>
#include <sst/elements/merlin/router.h>

#include <array>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace SST::Collective {
namespace {

using Request = SST::Interfaces::SimpleNetwork::Request;

void
require(bool condition, const char* message)
{
    if ( !condition ) throw std::runtime_error(message);
}

RouteIdV1
makeRoute()
{
    return { 1, 1 };
}

MerlinStaticCollectiveRouteProjection
makeLeafProjection()
{
    MerlinStaticCollectiveRouteProjection projection;
    projection.root                   = false;
    projection.parent_port            = 2;
    projection.subtree_representative = { 0, 0 };
    projection.root_representative    = { 0, 0 };
    projection.local_endpoint_branches.push_back({ 0, { 0, 0 } });
    projection.local_endpoint_branches.push_back({ 1, { 1, 1 } });
    return projection;
}

MerlinStaticCollectiveRouteProjection
makeIntermediateProjection()
{
    MerlinStaticCollectiveRouteProjection projection;
    projection.root                   = false;
    projection.parent_port            = 2;
    projection.subtree_representative = { 2, 2 };
    projection.root_representative    = { 0, 0 };
    projection.child_branches.push_back({ 0, { 2, 2 } });
    projection.local_endpoint_branches.push_back({ 1, { 4, 4 } });
    return projection;
}

MerlinStaticCollectiveRouteProjection
makeRootProjection()
{
    MerlinStaticCollectiveRouteProjection projection;
    projection.root                   = true;
    projection.subtree_representative = { 0, 0 };
    projection.root_representative    = { 0, 0 };
    projection.child_branches.push_back({ 0, { 0, 0 } });
    projection.child_branches.push_back({ 1, { 2, 2 } });
    return projection;
}

MerlinStaticCollectiveRouteProjection
makeOrderedRootProjection(size_t branch_count)
{
    MerlinStaticCollectiveRouteProjection projection;
    projection.root                   = true;
    projection.subtree_representative = { 0, 0 };
    projection.root_representative    = { 0, 0 };
    for ( size_t index = 0; index < branch_count; ++index ) {
        projection.child_branches.push_back(
            { static_cast<uint32_t>(index),
                { static_cast<int64_t>(index), static_cast<int64_t>(index) } });
    }
    return projection;
}

MerlinStaticCollectiveRouteProjection
makeSplitRepresentativeRootProjection()
{
    MerlinStaticCollectiveRouteProjection projection;
    projection.root                   = true;
    projection.subtree_representative = { 10, 100 };
    projection.root_representative    = { 10, 100 };
    projection.child_branches.push_back({ 0, { 20, 200 } });
    projection.child_branches.push_back({ 1, { 30, 300 } });
    return projection;
}

CollectiveServiceData*
makeData(uint64_t invocation, CollectiveDirection direction, double value)
{
    std::array<uint8_t, CollectiveServiceData::VALUE_BYTES> bytes {};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return new CollectiveServiceData(makeRoute(), invocation, direction, bytes);
}

std::unique_ptr<SST::Merlin::internal_router_event>
makeIngress(int64_t destination, int64_t logical_source, int64_t trusted_source,
    uint64_t invocation, CollectiveDirection direction, double value, int vn,
    int transport_vc = -1)
{
    auto request = std::make_unique<Request>(
        destination, logical_source, CollectiveServiceData::MODELED_REQUEST_BITS, true, true);
    request->vn             = vn;
    request->allow_adaptive = false;
    request->giveServiceData(makeData(invocation, direction, value));
    auto envelope = std::make_unique<SST::Merlin::RtrEvent>(request.release(), trusted_source, vn);
    require(envelope->setSyntheticTransportMetadata(13, 0), "test envelope metadata failed");
    auto event = std::make_unique<SST::Merlin::internal_router_event>(envelope.release());
    const int vc = transport_vc < 0 ? vn : transport_vc;
    event->setVC(vc);
    event->setCreditReturnVC(vc);
    return event;
}

class FakeHost final : public SST::Merlin::NetworkServiceHost
{
public:
    bool supportsNetworkServiceOutput(const SST::Merlin::NetworkServiceOutputSpec& spec) const override
    {
        return output_valid && spec.valid() && spec.route_vn < 2 && spec.output_port < 3 &&
               spec.output_vc == spec.route_vn &&
               spec.size_in_bits == CollectiveServiceData::MODELED_REQUEST_BITS &&
               spec.size_in_flits == 13;
    }

    bool tryEnqueueNetworkServiceOutput(SST::Merlin::NetworkServiceID service_id,
        SST::Merlin::NetworkServiceSyntheticPacket& packet) override
    {
        require(service_id == CollectiveServiceData::SERVICE_ID, "wrong service ID offered to host");
        if ( admitted.size() >= capacity ) return false;
        admitted.push_back(std::move(packet));
        return true;
    }

    void wakeNetworkServiceProcessor() override { ++wakes; }

    SST::Merlin::NetworkServiceSyntheticPacket pop()
    {
        require(!admitted.empty(), "fake host pop from empty queue");
        auto packet = std::move(admitted.front());
        admitted.pop_front();
        return packet;
    }

    size_t capacity = 0;
    uint64_t wakes = 0;
    bool output_valid = true;
    std::deque<SST::Merlin::NetworkServiceSyntheticPacket> admitted;
};

class FakeQueue final : public SST::Merlin::NetworkServiceIngressQueue
{
public:
    explicit FakeQueue(std::unique_ptr<SST::Merlin::internal_router_event> event) : event_(std::move(event)) {}

    const SST::Merlin::internal_router_event* inspectNetworkServiceHead(int vc) const override
    {
        return vc == vc_ ? event_.get() : nullptr;
    }

    SST::Merlin::internal_router_event* recvNetworkServiceExpected(
        int vc, const SST::Merlin::internal_router_event* expected) override
    {
        if ( expected == nullptr || inspectNetworkServiceHead(vc) != expected ) {
            return nullptr;
        }
        return event_.release();
    }

    int vc_ = 0;

private:
    std::unique_ptr<SST::Merlin::internal_router_event> event_;
};

SST::Merlin::NetworkServiceDecision
inspect(MerlinStaticCollectiveProcessor& processor, FakeQueue& queue, int port, int vc)
{
    queue.vc_ = vc;
    return processor.inspect({ port, vc, queue.inspectNetworkServiceHead(vc) });
}

void
accept(MerlinStaticCollectiveProcessor& processor, FakeQueue& queue, int port, int vc)
{
    const auto decision = inspect(processor, queue, port, vc);
    const auto expected = queue.inspectNetworkServiceHead(vc);
    require(decision.disposition == SST::Merlin::NetworkServiceDisposition::Accept,
        "valid collective ingress was not accepted");
    std::unique_ptr<SST::Merlin::internal_router_event> consumed(
        queue.recvNetworkServiceExpected(vc, expected));
    require(consumed != nullptr && consumed.get() == expected,
        "accepted collective transaction did not consume the expected head");
    processor.consume({ port, vc, std::move(consumed) });
}

void
accept(MerlinStaticCollectiveProcessor& processor, int port, int vc,
    std::unique_ptr<SST::Merlin::internal_router_event> event)
{
    FakeQueue queue(std::move(event));
    accept(processor, queue, port, vc);
}

void
reject(MerlinStaticCollectiveProcessor& processor, int port, int vc,
    std::unique_ptr<SST::Merlin::internal_router_event> event, const char* message)
{
    FakeQueue queue(std::move(event));
    queue.vc_ = vc;
    const auto before = queue.inspectNetworkServiceHead(vc);
    const auto decision = inspect(processor, queue, port, vc);
    const auto after = queue.inspectNetworkServiceHead(vc);
    require(decision.disposition == SST::Merlin::NetworkServiceDisposition::Reject, message);
    require(before != nullptr && after == before,
        "rejected collective ingress consumed or replaced the queue head");
}

double
packetValue(const SST::Merlin::NetworkServiceSyntheticPacket& packet, CollectiveDirection direction)
{
    require(packet.request != nullptr, "synthetic packet lost Request ownership");
    const auto* data = packet.request->inspectServiceDataAs<CollectiveServiceData>();
    require(data != nullptr && data->validFor(makeRoute(), direction, packet.request->size_in_bits),
        "synthetic packet has wrong collective sidecar");
    double value = 0.0;
    std::memcpy(&value, data->value.data(), sizeof(value));
    return value;
}

void
testLeafAndRetry()
{
    FakeHost host;
    MerlinStaticCollectiveProcessor processor(&host, makeRoute(), makeLeafProjection(), 2);

    accept(processor, 0, 0, makeIngress(0, 0, 0, 1, CollectiveDirection::Contribution, 1.0, 0));
    accept(processor, 1, 0, makeIngress(0, 1, 1, 1, CollectiveDirection::Contribution, 2.0, 0));
    require(processor.pendingEgressCount() == 1 && processor.hasActiveInvocation(),
        "leaf did not retain one upward aggregate and active key");

    require(!processor.progressPendingEgress() && processor.pendingEgressCount() == 1,
        "full host requester did not retain/retry upward aggregate");

    FakeQueue waiting_result(
        makeIngress(0, 0, 0, 1, CollectiveDirection::Result, 10.0, 1));
    waiting_result.vc_ = 1;
    const auto* waiting_head = waiting_result.inspectNetworkServiceHead(1);
    const auto first_busy = inspect(processor, waiting_result, 2, 1);
    const auto second_busy = inspect(processor, waiting_result, 2, 1);
    require(first_busy.disposition == SST::Merlin::NetworkServiceDisposition::Busy &&
                second_busy.disposition == first_busy.disposition &&
                second_busy.opaque_diagnostic == first_busy.opaque_diagnostic &&
                waiting_result.inspectNetworkServiceHead(1) == waiting_head &&
                processor.pendingEgressCount() == 1 && processor.hasActiveInvocation(),
        "repeat inspection changed an egress-full Busy head or processor state");

    host.capacity = 1;
    require(processor.progressPendingEgress(), "leaf upward aggregate did not drain after capacity opened");
    auto upward = host.pop();
    require(upward.output_port == 2 && upward.output_vc == 0 && upward.route_vn == 0 &&
                packetValue(upward, CollectiveDirection::Contribution) == 3.0,
        "leaf emitted the wrong upward aggregate");

    accept(processor, waiting_result, 2, 1);
    require(processor.pendingEgressCount() == 2, "leaf did not create one result per local branch");
    require(!processor.progressPendingEgress(), "depth-one host unexpectedly accepted two result packets");
    auto first = host.pop();
    require(processor.progressPendingEgress(), "second leaf result did not drain after first departed");
    auto second = host.pop();
    require(first.output_port == 0 && second.output_port == 1 &&
                packetValue(first, CollectiveDirection::Result) == 10.0 &&
                packetValue(second, CollectiveDirection::Result) == 10.0,
        "leaf result fanout is wrong or unordered");
    require(!processor.hasActiveInvocation(), "leaf did not retire after result fanout drained");

    FakeQueue stale(makeIngress(0, 0, 0, 1, CollectiveDirection::Contribution, 1.0, 0));
    auto duplicate = inspect(processor, stale, 0, 0);
    require(duplicate.disposition == SST::Merlin::NetworkServiceDisposition::Reject,
        "retired invocation reopened after completion");

}

void
testMalformedIngressDoesNotPoisonState()
{
    FakeHost host;
    host.capacity = 2;
    MerlinStaticCollectiveProcessor processor(&host, makeRoute(), makeLeafProjection(), 2);

    reject(processor, 0, 0,
        makeIngress(0, 0, 0, 6, CollectiveDirection::Contribution, 1.0, 1, 0),
        "contribution on the wrong logical VN was not rejected");
    require(!processor.hasActiveInvocation() && processor.pendingEgressCount() == 0,
        "wrong-VN rejection changed idle processor state");

    reject(processor, 0, 1,
        makeIngress(0, 0, 0, 6, CollectiveDirection::Result, 10.0, 1),
        "result arriving from a local contribution branch was not rejected");
    require(!processor.hasActiveInvocation() && processor.pendingEgressCount() == 0,
        "wrong-direction rejection changed idle processor state");

    accept(processor, 0, 0,
        makeIngress(0, 0, 0, 6, CollectiveDirection::Contribution, 1.0, 0));
    reject(processor, 0, 0,
        makeIngress(0, 0, 0, 6, CollectiveDirection::Contribution, 99.0, 0),
        "duplicate contribution from an active branch was not rejected");
    require(processor.hasActiveInvocation() && processor.pendingEgressCount() == 0,
        "active duplicate rejection damaged the partial reduction");

    accept(processor, 1, 0,
        makeIngress(0, 1, 1, 6, CollectiveDirection::Contribution, 2.0, 0));
    require(processor.progressPendingEgress(),
        "valid reduction did not progress after malformed ingress rejections");
    auto upward = host.pop();
    require(upward.output_port == 2 &&
                packetValue(upward, CollectiveDirection::Contribution) == 3.0,
        "malformed ingress changed the valid upward aggregate");

    accept(processor, 2, 1,
        makeIngress(0, 0, 0, 6, CollectiveDirection::Result, 10.0, 1));
    require(processor.progressPendingEgress() && host.admitted.size() == 2,
        "valid result fanout did not progress after malformed ingress rejections");
    auto first = host.pop();
    auto second = host.pop();
    require(packetValue(first, CollectiveDirection::Result) == 10.0 &&
                packetValue(second, CollectiveDirection::Result) == 10.0 &&
                !processor.hasActiveInvocation(),
        "processor did not complete correctly after malformed ingress rejections");

}

void
testInvalidStaticOutputRejectedAtInstall()
{
    FakeHost host;
    host.output_valid = false;
    bool rejected = false;
    try {
        MerlinStaticCollectiveProcessor processor(&host, makeRoute(), makeLeafProjection(), 2);
    }
    catch ( const std::invalid_argument& ) {
        rejected = true;
    }
    require(rejected, "permanently invalid synthetic output configuration was installed");
}

void
testIntermediate()
{
    FakeHost host;
    host.capacity = 2;
    MerlinStaticCollectiveProcessor processor(&host, makeRoute(), makeIntermediateProjection(), 2);
    accept(processor, 0, 0, makeIngress(0, 2, 2, 2, CollectiveDirection::Contribution, 2.0, 0));
    accept(processor, 1, 0, makeIngress(0, 4, 4, 2, CollectiveDirection::Contribution, 4.0, 0));
    require(processor.progressPendingEgress(), "intermediate upward aggregate did not drain");
    auto upward = host.pop();
    require(upward.output_port == 2 && upward.trusted_src == 2 &&
                packetValue(upward, CollectiveDirection::Contribution) == 6.0,
        "intermediate emitted the wrong ordered aggregate");

    accept(processor, 2, 1, makeIngress(2, 0, 0, 2, CollectiveDirection::Result, 10.0, 1));
    require(processor.progressPendingEgress() && host.admitted.size() == 2,
        "intermediate result fanout did not drain");
    auto child = host.pop();
    auto local = host.pop();
    require(child.output_port == 0 && local.output_port == 1 &&
                packetValue(child, CollectiveDirection::Result) == 10.0 &&
                packetValue(local, CollectiveDirection::Result) == 10.0,
        "intermediate result fanout is wrong");
}

void
testRoot()
{
    FakeHost host;
    host.capacity = 2;
    MerlinStaticCollectiveProcessor processor(&host, makeRoute(), makeRootProjection(), 2);
    accept(processor, 0, 0, makeIngress(0, 0, 0, 3, CollectiveDirection::Contribution, 3.0, 0));
    accept(processor, 1, 0, makeIngress(0, 2, 2, 3, CollectiveDirection::Contribution, 7.0, 0));
    require(processor.progressPendingEgress() && host.admitted.size() == 2,
        "root result fanout did not enter synthetic arbitration");
    auto left  = host.pop();
    auto right = host.pop();
    require(left.output_port == 0 && right.output_port == 1 &&
                packetValue(left, CollectiveDirection::Result) == 10.0 &&
                packetValue(right, CollectiveDirection::Result) == 10.0,
        "root computed or fanned out the wrong result");
    require(!processor.hasActiveInvocation(), "root did not retire its completed invocation");
}

void
testStrictOrderedArithmetic()
{
    FakeHost host;
    host.capacity = 3;
    MerlinStaticCollectiveProcessor processor(
        &host, makeRoute(), makeOrderedRootProjection(3), 3);

    // Arrival order differs from branch order.  The required binary64 fold is
    // ((1e16 + 1) + -1e16) == 0, not an arrival-ordered or reassociated sum.
    accept(processor, 2, 0,
        makeIngress(0, 2, 2, 7, CollectiveDirection::Contribution, -1.0e16, 0));
    accept(processor, 0, 0,
        makeIngress(0, 0, 0, 7, CollectiveDirection::Contribution, 1.0e16, 0));
    accept(processor, 1, 0,
        makeIngress(0, 1, 1, 7, CollectiveDirection::Contribution, 1.0, 0));
    require(processor.progressPendingEgress() && host.admitted.size() == 3,
        "ordered root result fanout did not drain");
    while ( !host.admitted.empty() ) {
        require(packetValue(host.pop(), CollectiveDirection::Result) == 0.0,
            "root reduction was reassociated or folded in arrival order");
    }

    FakeHost single_host;
    single_host.capacity = 1;
    MerlinStaticCollectiveProcessor single(
        &single_host, makeRoute(), makeOrderedRootProjection(1), 1);
    accept(single, 0, 0,
        makeIngress(0, 0, 0, 8, CollectiveDirection::Contribution, 4.5, 0));
    require(single.progressPendingEgress() &&
                packetValue(single_host.pop(), CollectiveDirection::Result) == 4.5,
        "single-branch reduction changed its contribution");
}

void
testSplitPhysicalAndLogicalRepresentatives()
{
    FakeHost host;
    host.capacity = 2;
    MerlinStaticCollectiveProcessor processor(
        &host, makeRoute(), makeSplitRepresentativeRootProjection(), 2);

    FakeQueue wrong_destination(
        makeIngress(100, 200, 20, 4, CollectiveDirection::Contribution, 4.0, 0));
    require(inspect(processor, wrong_destination, 0, 0).disposition ==
                SST::Merlin::NetworkServiceDisposition::Reject,
        "router accepted caller-visible destination in place of physical destination");

    accept(processor, 0, 0, makeIngress(10, 200, 20, 4, CollectiveDirection::Contribution, 4.0, 0));
    accept(processor, 1, 0, makeIngress(10, 300, 30, 4, CollectiveDirection::Contribution, 6.0, 0));
    require(processor.progressPendingEgress() && host.admitted.size() == 2,
        "split-representative root result fanout did not drain");
    auto left  = host.pop();
    auto right = host.pop();
    require(left.request && right.request && left.request->dest == 20 && right.request->dest == 30 &&
                left.request->src == 100 && right.request->src == 100 && left.trusted_src == 10 &&
                right.trusted_src == 10 && packetValue(left, CollectiveDirection::Result) == 10.0 &&
                packetValue(right, CollectiveDirection::Result) == 10.0,
        "processor confused physical transport NIDs with caller-visible logical NIDs");

    accept(processor, 0, 0, makeIngress(10, 200, 20, 5, CollectiveDirection::Contribution, 1.0, 0));
    FakeQueue retired_while_active(
        makeIngress(10, 300, 30, 4, CollectiveDirection::Contribution, 6.0, 0));
    require(inspect(processor, retired_while_active, 1, 0).disposition ==
                SST::Merlin::NetworkServiceDisposition::Reject,
        "retired invocation replay became persistent Busy behind a newer active key");
    accept(processor, 1, 0, makeIngress(10, 300, 30, 5, CollectiveDirection::Contribution, 2.0, 0));
    require(processor.progressPendingEgress() && host.admitted.size() == 2,
        "new invocation did not complete after rejecting a retired replay");
}

} // namespace

class MerlinStaticCollectiveProcessorTest final : public SST::Component
{
public:
    SST_ELI_REGISTER_COMPONENT(MerlinStaticCollectiveProcessorTest, "merlin",
        "collective_static_processor_contract_test", SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Focused contract test for the experimental static Merlin collective processor",
        COMPONENT_CATEGORY_UNCATEGORIZED)

    MerlinStaticCollectiveProcessorTest(SST::ComponentId_t id, SST::Params& params) : SST::Component(id)
    {
        (void)params;
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
        SST::Output output("", 1, 0, SST::Output::STDOUT);
        try {
            testInvalidStaticOutputRejectedAtInstall();
            testLeafAndRetry();
            testMalformedIngressDoesNotPoisonState();
            testIntermediate();
            testRoot();
            testStrictOrderedArithmetic();
            testSplitPhysicalAndLogicalRepresentatives();
        }
        catch ( const std::exception& error ) {
            output.fatal(CALL_INFO, 1, "Static Merlin collective processor contract FAIL: %s\n", error.what());
        }
        output.output("Static Merlin collective processor contract PASS\n");
        primaryComponentOKToEndSim();
    }
};

} // namespace SST::Collective
