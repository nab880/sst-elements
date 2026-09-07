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

// These functional fixtures isolate routing/state semantics from the reducer
// timing exercised explicitly in testReductionTiming below.
constexpr uint32_t FUNCTIONAL_REDUCTION_WIDTH = UINT32_MAX;

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
    std::vector<uint8_t> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return new CollectiveServiceData(
        makeRoute(), invocation, direction, STATIC_COLLECTIVE_SIGNATURE_V1, 0, bytes);
}

std::unique_ptr<SST::Merlin::internal_router_event>
makeIngress(int64_t destination, int64_t logical_source, int64_t trusted_source,
    uint64_t invocation, CollectiveDirection direction, double value, int vn)
{
    auto request = std::make_unique<Request>(
        destination, logical_source, STATIC_COLLECTIVE_REQUEST_BITS, true, true);
    request->vn             = vn;
    request->allow_adaptive = false;
    request->giveServiceData(makeData(invocation, direction, value));
    auto envelope = std::make_unique<SST::Merlin::RtrEvent>(request.release(), trusted_source, vn);
    require(envelope->setSyntheticTransportMetadata(13, 0), "test envelope metadata failed");
    auto event = std::make_unique<SST::Merlin::internal_router_event>(envelope.release());
    event->setVC(vn);
    event->setCreditReturnVC(vn);
    return event;
}

class FakeHost final : public SST::Merlin::NetworkServiceHost
{
public:
    bool supportsNetworkServiceOutput(const SST::Merlin::NetworkServiceOutputSpec& spec) const override
    {
        return output_valid && spec.valid() && spec.route_vn < 4 && spec.output_port < 3 &&
               spec.size_in_bits == STATIC_COLLECTIVE_REQUEST_BITS;
    }

    bool canEnqueueNetworkServiceOutput(const SST::Merlin::NetworkServiceOutputSpec& spec) const override
    {
        return supportsNetworkServiceOutput(spec) && admitted.size() < capacity &&
               blocked_port != spec.output_port;
    }

    bool tryEnqueueNetworkServiceOutput(SST::Merlin::NetworkServiceID service_id,
        SST::Merlin::NetworkServiceSyntheticPacket& packet) override
    {
        require(service_id == CollectiveServiceData::SERVICE_ID, "wrong service ID offered to host");
        ++enqueue_attempts;
        if ( admitted.size() >= capacity || blocked_port == packet.output_port ) return false;
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
    uint64_t enqueue_attempts = 0;
    int blocked_port = -1;
    bool output_valid = true;
    std::deque<SST::Merlin::NetworkServiceSyntheticPacket> admitted;
};

SST::Merlin::NetworkServiceDecision
inspect(MerlinStaticCollectiveProcessor& processor, const std::unique_ptr<SST::Merlin::internal_router_event>& event,
    int port, int vn)
{
    return processor.inspect({ port, vn, event.get() });
}

void
accept(MerlinStaticCollectiveProcessor& processor, int port, int vn,
    std::unique_ptr<SST::Merlin::internal_router_event> event)
{
    require(inspect(processor, event, port, vn).disposition == SST::Merlin::NetworkServiceDisposition::Accept,
        "valid collective ingress was not accepted");
    processor.consume({ port, vn, std::move(event) });
}

void
reject(MerlinStaticCollectiveProcessor& processor, int port, int vn,
    std::unique_ptr<SST::Merlin::internal_router_event> event, const char* message)
{
    require(inspect(processor, event, port, vn).disposition == SST::Merlin::NetworkServiceDisposition::Reject,
        message);
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
    MerlinStaticCollectiveProcessor processor(&host, makeRoute(), makeLeafProjection(), 0, 1, FUNCTIONAL_REDUCTION_WIDTH, 0);

    accept(processor, 0, 0, makeIngress(0, 0, 0, 1, CollectiveDirection::Contribution, 1.0, 0));
    accept(processor, 1, 0, makeIngress(0, 1, 1, 1, CollectiveDirection::Contribution, 2.0, 0));
    require(host.wakes == 1 && processor.hasScheduledWork() && !processor.progress(),
        "final contribution did not wake the router and retain/retry blocked egress");

    auto waiting_result = makeIngress(0, 0, 0, 1, CollectiveDirection::Result, 10.0, 1);
    const auto first_busy = inspect(processor, waiting_result, 2, 1);
    const auto second_busy = inspect(processor, waiting_result, 2, 1);
    require(first_busy.disposition == SST::Merlin::NetworkServiceDisposition::Busy &&
                second_busy.disposition == first_busy.disposition &&
                second_busy.opaque_diagnostic == first_busy.opaque_diagnostic,
        "repeat inspection changed a pending-upward Busy head or processor state");

    host.capacity = 1;
    require(processor.progress(), "leaf upward aggregate did not drain after capacity opened");
    auto upward = host.pop();
    require(upward.output_port == 2 && upward.route_vn == 0 &&
                packetValue(upward, CollectiveDirection::Contribution) == 3.0,
        "leaf emitted the wrong upward aggregate");

    accept(processor, 2, 1, std::move(waiting_result));
    require(!processor.progress(), "depth-one host unexpectedly accepted two result packets");
    auto first = host.pop();
    require(processor.progress(), "second leaf result did not drain after first departed");
    auto second = host.pop();
    require(first.output_port == 0 && second.output_port == 1 &&
                packetValue(first, CollectiveDirection::Result) == 10.0 &&
                packetValue(second, CollectiveDirection::Result) == 10.0,
        "leaf result fanout is wrong or unordered");

    reject(processor, 0, 0, makeIngress(0, 0, 0, 1, CollectiveDirection::Contribution, 1.0, 0),
        "retired invocation reopened after completion");

}

void
testMalformedIngressDoesNotPoisonState()
{
    FakeHost host;
    host.capacity = 2;
    MerlinStaticCollectiveProcessor processor(&host, makeRoute(), makeLeafProjection(), 0, 1, FUNCTIONAL_REDUCTION_WIDTH, 0);

    reject(processor, 0, 1,
        makeIngress(0, 0, 0, 6, CollectiveDirection::Contribution, 1.0, 1),
        "contribution arriving on the result VN was not rejected");

    reject(processor, 0, 1,
        makeIngress(0, 0, 0, 6, CollectiveDirection::Result, 10.0, 1),
        "result arriving from a local contribution branch was not rejected");

    accept(processor, 0, 0,
        makeIngress(0, 0, 0, 6, CollectiveDirection::Contribution, 1.0, 0));
    reject(processor, 0, 0,
        makeIngress(0, 0, 0, 6, CollectiveDirection::Contribution, 99.0, 0),
        "duplicate contribution from an active branch was not rejected");

    accept(processor, 1, 0,
        makeIngress(0, 1, 1, 6, CollectiveDirection::Contribution, 2.0, 0));
    require(processor.progress(),
        "valid reduction did not progress after malformed ingress rejections");
    auto upward = host.pop();
    require(upward.output_port == 2 &&
                packetValue(upward, CollectiveDirection::Contribution) == 3.0,
        "malformed ingress changed the valid upward aggregate");

    accept(processor, 2, 1,
        makeIngress(0, 0, 0, 6, CollectiveDirection::Result, 10.0, 1));
    require(processor.progress() && host.admitted.size() == 2,
        "valid result fanout did not progress after malformed ingress rejections");
    auto first = host.pop();
    auto second = host.pop();
    require(packetValue(first, CollectiveDirection::Result) == 10.0 &&
                packetValue(second, CollectiveDirection::Result) == 10.0,
        "processor did not complete correctly after malformed ingress rejections");

}

void
testInvalidStaticOutputRejectedAtInstall()
{
    FakeHost host;
    host.output_valid = false;
    bool rejected = false;
    try {
        MerlinStaticCollectiveProcessor processor(&host, makeRoute(), makeLeafProjection(), 0, 1, FUNCTIONAL_REDUCTION_WIDTH, 0);
    }
    catch ( const std::invalid_argument& ) {
        rejected = true;
    }
    require(rejected, "permanently invalid synthetic output configuration was installed");

    FakeHost valid_host;
    bool same_vn_rejected = false;
    try {
        MerlinStaticCollectiveProcessor processor(&valid_host, makeRoute(), makeLeafProjection(), 1, 1);
    }
    catch ( const std::invalid_argument& ) {
        same_vn_rejected = true;
    }
    require(same_vn_rejected, "identical reduce and result VNs were installed");
    bool zero_width_rejected = false;
    try {
        MerlinStaticCollectiveProcessor processor(&valid_host, makeRoute(), makeLeafProjection(), 0, 1, 0, 1);
    }
    catch ( const std::invalid_argument& ) {
        zero_width_rejected = true;
    }
    require(zero_width_rejected, "zero-throughput reduction would leave an invocation stalled forever");
}

void
testIntermediate()
{
    FakeHost host;
    host.capacity = 2;
    MerlinStaticCollectiveProcessor processor(&host, makeRoute(), makeIntermediateProjection(), 0, 1, FUNCTIONAL_REDUCTION_WIDTH, 0);
    accept(processor, 0, 0, makeIngress(0, 2, 2, 2, CollectiveDirection::Contribution, 2.0, 0));
    accept(processor, 1, 0, makeIngress(0, 4, 4, 2, CollectiveDirection::Contribution, 4.0, 0));
    require(processor.progress(), "intermediate upward aggregate did not drain");
    auto upward = host.pop();
    require(upward.output_port == 2 && upward.trusted_src == 2 &&
                packetValue(upward, CollectiveDirection::Contribution) == 6.0,
        "intermediate emitted the wrong ordered aggregate");

    accept(processor, 2, 1, makeIngress(2, 0, 0, 2, CollectiveDirection::Result, 10.0, 1));
    require(processor.progress() && host.admitted.size() == 2,
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
    MerlinStaticCollectiveProcessor processor(&host, makeRoute(), makeRootProjection(), 0, 1, FUNCTIONAL_REDUCTION_WIDTH, 0);
    accept(processor, 0, 0, makeIngress(0, 0, 0, 3, CollectiveDirection::Contribution, 3.0, 0));
    accept(processor, 1, 0, makeIngress(0, 2, 2, 3, CollectiveDirection::Contribution, 7.0, 0));
    require(processor.progress() && host.admitted.size() == 2,
        "root result fanout did not enter synthetic arbitration");
    auto left  = host.pop();
    auto right = host.pop();
    require(left.output_port == 0 && right.output_port == 1 &&
                packetValue(left, CollectiveDirection::Result) == 10.0 &&
                packetValue(right, CollectiveDirection::Result) == 10.0,
        "root computed or fanned out the wrong result");
    reject(processor, 0, 0, makeIngress(0, 0, 0, 3, CollectiveDirection::Contribution, 3.0, 0),
        "root did not retire its completed invocation");
}

void
testStrictOrderedArithmetic()
{
    FakeHost host;
    host.capacity = 3;
    MerlinStaticCollectiveProcessor processor(
        &host, makeRoute(), makeOrderedRootProjection(3), 0, 1, FUNCTIONAL_REDUCTION_WIDTH, 0);

    // Arrival order differs from branch order.  The required binary64 fold is
    // ((1e16 + 1) + -1e16) == 0, not an arrival-ordered or reassociated sum.
    accept(processor, 2, 0,
        makeIngress(0, 2, 2, 7, CollectiveDirection::Contribution, -1.0e16, 0));
    accept(processor, 0, 0,
        makeIngress(0, 0, 0, 7, CollectiveDirection::Contribution, 1.0e16, 0));
    accept(processor, 1, 0,
        makeIngress(0, 1, 1, 7, CollectiveDirection::Contribution, 1.0, 0));
    require(processor.progress() && host.admitted.size() == 3,
        "ordered root result fanout did not drain");
    while ( !host.admitted.empty() ) {
        require(packetValue(host.pop(), CollectiveDirection::Result) == 0.0,
            "root reduction was reassociated or folded in arrival order");
    }

    FakeHost single_host;
    single_host.capacity = 1;
    MerlinStaticCollectiveProcessor single(
        &single_host, makeRoute(), makeOrderedRootProjection(1), 0, 1, FUNCTIONAL_REDUCTION_WIDTH, 0);
    accept(single, 0, 0,
        makeIngress(0, 0, 0, 8, CollectiveDirection::Contribution, 4.5, 0));
    require(single.progress() &&
                packetValue(single_host.pop(), CollectiveDirection::Result) == 4.5,
        "single-branch reduction changed its contribution");
}

void
testSplitPhysicalAndLogicalRepresentatives()
{
    FakeHost host;
    host.capacity = 2;
    MerlinStaticCollectiveProcessor processor(
        &host, makeRoute(), makeSplitRepresentativeRootProjection(), 0, 1, FUNCTIONAL_REDUCTION_WIDTH, 0);

    reject(processor, 0, 0, makeIngress(100, 200, 20, 4, CollectiveDirection::Contribution, 4.0, 0),
        "router accepted caller-visible destination in place of physical destination");

    accept(processor, 0, 0, makeIngress(10, 200, 20, 4, CollectiveDirection::Contribution, 4.0, 0));
    accept(processor, 1, 0, makeIngress(10, 300, 30, 4, CollectiveDirection::Contribution, 6.0, 0));
    require(processor.progress() && host.admitted.size() == 2,
        "split-representative root result fanout did not drain");
    auto left  = host.pop();
    auto right = host.pop();
    require(left.request && right.request && left.request->dest == 20 && right.request->dest == 30 &&
                left.request->src == 100 && right.request->src == 100 && left.trusted_src == 10 &&
                right.trusted_src == 10 && packetValue(left, CollectiveDirection::Result) == 10.0 &&
                packetValue(right, CollectiveDirection::Result) == 10.0,
        "processor confused physical transport NIDs with caller-visible logical NIDs");

    accept(processor, 0, 0, makeIngress(10, 200, 20, 5, CollectiveDirection::Contribution, 1.0, 0));
    reject(processor, 1, 0, makeIngress(10, 300, 30, 4, CollectiveDirection::Contribution, 6.0, 0),
        "retired invocation replay became persistent Busy behind a newer active key");
    accept(processor, 1, 0, makeIngress(10, 300, 30, 5, CollectiveDirection::Contribution, 2.0, 0));
    require(processor.progress() && host.admitted.size() == 2,
        "new invocation did not complete after rejecting a retired replay");
}

void
testConfiguredServiceVNs()
{
    // The processor owns whichever VNs it is configured with; nothing about
    // the reduce and result VNs is pinned to 0 and 1.
    FakeHost host;
    host.capacity = 2;
    MerlinStaticCollectiveProcessor processor(&host, makeRoute(), makeLeafProjection(), 2, 3, FUNCTIONAL_REDUCTION_WIDTH, 0);
    require(processor.ownedVNs() == std::vector<int> { 2, 3 }, "processor did not own its configured VNs");

    reject(processor, 0, 0, makeIngress(0, 0, 0, 9, CollectiveDirection::Contribution, 1.0, 0),
        "contribution on VN 0 was accepted by a processor configured for VN 2");
    accept(processor, 0, 2, makeIngress(0, 0, 0, 9, CollectiveDirection::Contribution, 1.0, 2));
    accept(processor, 1, 2, makeIngress(0, 1, 1, 9, CollectiveDirection::Contribution, 2.0, 2));
    require(processor.progress(), "configured-VN upward aggregate did not drain");
    auto upward = host.pop();
    require(upward.route_vn == 2 && upward.request->vn == 2 &&
                packetValue(upward, CollectiveDirection::Contribution) == 3.0,
        "upward aggregate did not use the configured reduce VN");

    reject(processor, 2, 1, makeIngress(0, 0, 0, 9, CollectiveDirection::Result, 10.0, 1),
        "result on VN 1 was accepted by a processor configured for VN 3");
    accept(processor, 2, 3, makeIngress(0, 0, 0, 9, CollectiveDirection::Result, 10.0, 3));
    require(processor.progress() && host.admitted.size() == 2,
        "configured-VN result fanout did not drain");
    auto first  = host.pop();
    auto second = host.pop();
    require(first.route_vn == 3 && second.route_vn == 3 && first.request->vn == 3 &&
                packetValue(first, CollectiveDirection::Result) == 10.0,
        "result fanout did not use the configured result VN");
}

void
testReductionTiming()
{
    // Three branches need two additions.  Width one plus two cycles of
    // result latency must produce its first output on precisely tick four;
    // width two shortens that to tick three without changing arithmetic.
    for ( uint32_t width : { uint32_t { 1 }, uint32_t { 2 } } ) {
        FakeHost host;
        host.capacity = 3;
        MerlinStaticCollectiveProcessor processor(
            &host, makeRoute(), makeOrderedRootProjection(3), 0, 1, width, 2);
        accept(processor, 2, 0, makeIngress(0, 2, 2, 20, CollectiveDirection::Contribution, 3.0, 0));
        accept(processor, 0, 0, makeIngress(0, 0, 0, 20, CollectiveDirection::Contribution, 1.0, 0));
        require(!processor.hasScheduledWork(), "incomplete fan-in scheduled arithmetic");
        accept(processor, 1, 0, makeIngress(0, 1, 1, 20, CollectiveDirection::Contribution, 2.0, 0));
        require(processor.hasScheduledWork() && host.admitted.empty(),
            "final contribution bypassed clock-driven reduction");
        auto future = makeIngress(0, 0, 0, 21, CollectiveDirection::Contribution, 1.0, 0);
        require(inspect(processor, future, 0, 0).disposition == SST::Merlin::NetworkServiceDisposition::Busy,
            "another invocation displaced scheduled reduction");
        const uint32_t ready_tick = 2 / width + 2;
        for ( uint32_t tick = 1; tick < ready_tick; ++tick ) {
            require(!processor.progress() && processor.hasScheduledWork() && host.admitted.empty(),
                "reducer emitted before configured throughput and latency elapsed");
        }
        require(processor.progress() && !processor.hasScheduledWork() && host.admitted.size() == 3,
            "reducer missed its configured result-ready cycle");
        while ( !host.admitted.empty() ) {
            require(packetValue(host.pop(), CollectiveDirection::Result) == 6.0,
                "timed reducer changed the scalar result");
        }
    }

    FakeHost host;
    host.capacity = 2;
    MerlinStaticCollectiveProcessor defaults(&host, makeRoute(), makeRootProjection());
    accept(defaults, 0, 0, makeIngress(0, 0, 0, 22, CollectiveDirection::Contribution, 1.0, 0));
    accept(defaults, 1, 0, makeIngress(0, 2, 2, 22, CollectiveDirection::Contribution, 2.0, 0));
    require(!defaults.progress() && host.admitted.empty(), "default reducer omitted its latency cycle");
    require(defaults.progress() && host.admitted.size() == 2,
        "default reducer did not emit after one addition and one latency cycle");
}

void
testLazyFanoutSkipsBlockedOutput()
{
    FakeHost host;
    // A capacity-one host must support three result branches.
    host.capacity = 1;
    host.blocked_port = 0;
    MerlinStaticCollectiveProcessor processor(
        &host, makeRoute(), makeOrderedRootProjection(3), 0, 1, FUNCTIONAL_REDUCTION_WIDTH, 0);
    for ( int port = 0; port < 3; ++port ) {
        accept(processor, port, 0,
            makeIngress(0, port, port, 30, CollectiveDirection::Contribution, port + 1.0, 0));
    }
    require(!processor.progress() && host.admitted.size() == 1,
        "blocked first fanout branch hid a ready destination");
    require(host.enqueue_attempts == 1, "processor constructed/enqueued packets without host capacity");
    auto first = host.pop();
    require(first.output_port == 1 && packetValue(first, CollectiveDirection::Result) == 6.0,
        "lazy fanout did not bypass its blocked first output");
    require(!processor.progress(), "fanout retired while a destination was blocked");
    auto second = host.pop();
    require(second.output_port == 2 && packetValue(second, CollectiveDirection::Result) == 6.0,
        "lazy fanout duplicated an already-sent destination");
    const auto attempts = host.enqueue_attempts;
    require(!processor.progress() && host.admitted.empty() && host.enqueue_attempts == attempts,
        "blocked fanout allocated a packet instead of retaining only the scalar");
    host.blocked_port = -1;
    require(processor.progress() && !processor.hasScheduledWork(),
        "lazy fanout did not finish when its final output became available");
    auto final = host.pop();
    require(final.output_port == 0 && packetValue(final, CollectiveDirection::Result) == 6.0,
        "lazy fanout lost its deferred destination or result");
}

void
testQuiescentRestoreAfterInvocation()
{
    FakeHost original_host;
    original_host.capacity = 3;
    MerlinStaticCollectiveProcessor original(
        &original_host, makeRoute(), makeOrderedRootProjection(3), 2, 3, 1, 2);
    for ( int port = 0; port < 3; ++port ) {
        accept(original, port, 2,
            makeIngress(0, port, port, 41, CollectiveDirection::Contribution, port + 1.0, 2));
    }
    for ( int tick = 0; tick < 3; ++tick ) {
        require(!original.progress() && original_host.admitted.empty(),
            "pre-checkpoint invocation ignored configured reduction timing");
    }
    require(original.progress() && !original.hasScheduledWork() && original_host.admitted.size() == 3,
        "pre-checkpoint invocation did not become quiescent");
    for ( int port = 0; port < 3; ++port ) {
        auto packet = original_host.pop();
        require(packet.output_port == port && packetValue(packet, CollectiveDirection::Result) == 6.0,
            "pre-checkpoint invocation emitted an incorrect result");
    }

    SST::Core::Serialization::serializer ser;
    ser.start_sizing();
    original.serialize_order(ser);
    std::vector<char> wire(ser.size());
    ser.start_packing(wire.data(), wire.size());
    original.serialize_order(ser);
    MerlinStaticCollectiveProcessor restored;
    ser.start_unpacking(wire.data(), wire.size());
    restored.serialize_order(ser);

    FakeHost restored_host;
    restored_host.capacity = 1;
    restored_host.blocked_port = 0;
    restored.bindHost(&restored_host);
    require(restored.validateInstalledTransport() && !restored.hasScheduledWork() &&
                restored.ownedVNs() == std::vector<int>({ 2, 3 }),
        "restored processor lost its installed transport or quiescent state");
    reject(restored, 0, 2, makeIngress(0, 0, 0, 41, CollectiveDirection::Contribution, 99.0, 2),
        "restored processor lost its retired invocation watermark");

    for ( int port = 0; port < 3; ++port ) {
        accept(restored, port, 2,
            makeIngress(0, port, port, 42, CollectiveDirection::Contribution, port + 4.0, 2));
    }
    for ( int tick = 0; tick < 3; ++tick ) {
        require(!restored.progress() && restored_host.admitted.empty(),
            "restored processor lost configured reduction throughput or latency");
    }
    for ( int port : { 1, 2 } ) {
        require(!restored.progress() && restored_host.admitted.size() == 1,
            "restored fanout failed to progress past a blocked output");
        auto packet = restored_host.pop();
        require(packet.output_port == port && packet.route_vn == 3 && packet.request->dest == port &&
                    packetValue(packet, CollectiveDirection::Result) == 15.0 &&
                    packet.request->inspectServiceDataAs<CollectiveServiceData>()->invocation_id == 42,
            "restored fanout emitted a stale, duplicate, or misrouted result");
    }
    require(!restored.progress() && restored_host.admitted.empty(),
        "restored fanout retired with a blocked destination");
    restored_host.blocked_port = -1;
    require(restored.progress() && !restored.hasScheduledWork() && restored_host.admitted.size() == 1,
        "restored fanout did not finish after backpressure cleared");
    auto final = restored_host.pop();
    require(final.output_port == 0 && final.route_vn == 3 && final.request->dest == 0 &&
                packetValue(final, CollectiveDirection::Result) == 15.0,
        "restored fanout lost its deferred branch or scalar value");
    reject(restored, 0, 2, makeIngress(0, 0, 0, 42, CollectiveDirection::Contribution, 99.0, 2),
        "restored processor did not retire the subsequent invocation");
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
            testConfiguredServiceVNs();
            testReductionTiming();
            testLazyFanoutSkipsBlockedOutput();
            testQuiescentRestoreAfterInvocation();
        }
        catch ( const std::exception& error ) {
            output.fatal(CALL_INFO, 1, "Static Merlin collective processor contract FAIL: %s\n", error.what());
        }
        output.output("Static Merlin collective processor contract PASS\n");
        primaryComponentOKToEndSim();
    }
};

} // namespace SST::Collective
