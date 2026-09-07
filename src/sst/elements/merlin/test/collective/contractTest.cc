// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "sst_config.h"

#include <sst/elements/merlin/hr_router/hr_router.h>
#include <sst/elements/merlin/hr_router/xbar_arb_lru.h>
#include <sst/elements/merlin/interfaces/ExtendedRequest.h>
#include <sst/elements/merlin/networkService.h>
#include <sst/elements/merlin/services/collective/collectiveServiceData.h>
#include <sst/elements/merlin/services/collective/staticCollectiveEndpoint.h>

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/link.h>
#include <sst/core/output.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace SST::Merlin::Test {

using SimpleNetwork = SST::Interfaces::SimpleNetwork;
constexpr SimpleNetwork::NetworkServiceID SERVICE_ID = SimpleNetwork::NETWORK_SERVICE_PLUGIN_MIN;
constexpr SimTime_t BUSY_RELEASE_NS = 4;

enum class Action : uint8_t { Pass = 1, Busy = 2 };

class ProbeData final : public SimpleNetwork::NetworkServiceData
{
public:
    static constexpr SimpleNetwork::NetworkServiceID SERVICE_ID = Test::SERVICE_ID;
    static constexpr SimpleNetwork::NetworkServiceDataToken DATA_TOKEN = 1;
    static constexpr SimpleNetwork::NetworkServiceVersion MIN_SCHEMA_VERSION = 1;
    static constexpr SimpleNetwork::NetworkServiceVersion MAX_SCHEMA_VERSION = 1;

    ProbeData() = default;
    ProbeData(Action action, uint32_t sequence) : action_(action), sequence_(sequence) {}

    SimpleNetwork::NetworkServiceID serviceID() const override { return SERVICE_ID; }
    SimpleNetwork::NetworkServiceDataToken dataToken() const override { return DATA_TOKEN; }
    SimpleNetwork::NetworkServiceVersion schemaVersion() const override { return MIN_SCHEMA_VERSION; }
    ProbeData* clone() const override { return new ProbeData(*this); }
    Action action() const { return action_; }
    uint32_t sequence() const { return sequence_; }

    void serialize_order(SST::Core::Serialization::serializer& ser) override
    {
        SST_SER(action_);
        SST_SER(sequence_);
    }

private:
    Action action_ = static_cast<Action>(0);
    uint32_t sequence_ = 0;
    ImplementSerializable(SST::Merlin::Test::ProbeData);
};

void require(bool condition, const char* message)
{
    if ( !condition ) throw std::runtime_error(message);
}

class ProbeProcessor final : public NetworkServiceProcessor
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(ProbeProcessor, "merlin", "network_service_probe_processor",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Minimal network-service integration processor",
        SST::Merlin::NetworkServiceProcessor)
    SST_ELI_DOCUMENT_PARAMS()

    ProbeProcessor(ComponentId_t id, Params&, NetworkServiceHost* host) : NetworkServiceProcessor(id, host)
    {
        if ( host == nullptr ) getSimulationOutput().fatal(CALL_INFO, 1, "probe processor requires a host\n");
    }
    ProbeProcessor() = default;

    std::vector<int> ownedVNs() const override { return { 0 }; }

    NetworkServiceID getServiceID() const override { return SERVICE_ID; }
    NetworkServiceRequestContract getRequestContract() const override
    {
        return { SERVICE_ID, ProbeData::DATA_TOKEN, ProbeData::MIN_SCHEMA_VERSION,
            ProbeData::MAX_SCHEMA_VERSION };
    }

    NetworkServiceDecision inspect(const NetworkServiceIngress& ingress) const override
    {
        const auto* request = ingress.event == nullptr ? nullptr : ingress.event->inspectRequest();
        const auto* data = request == nullptr ? nullptr : request->inspectServiceDataAs<ProbeData>();
        if ( ingress.input_port < 0 || ingress.input_vn != 0 || data == nullptr ||
             data->action() != Action::Busy ) {
            return { NetworkServiceDisposition::Reject, 1 };
        }
        return { getCurrentSimTimeNano() < BUSY_RELEASE_NS ? NetworkServiceDisposition::Busy :
                                                             NetworkServiceDisposition::Accept };
    }

    // Deliver the accepted packet to its destination as a synthetic packet
    // with identical service data, proving consume/re-emit conserves it.
    void consume(NetworkServiceOwnedIngress ingress) noexcept override
    {
        const auto* request = ingress.event == nullptr ? nullptr : ingress.event->inspectRequest();
        const auto* data = request == nullptr ? nullptr : request->inspectServiceDataAs<ProbeData>();
        if ( data == nullptr ) getSimulationOutput().fatal(CALL_INFO, 1, "probe consumed an invalid packet\n");
        auto echo = std::make_unique<SimpleNetwork::Request>(
            request->dest, request->src, request->size_in_bits, true, true);
        echo->vn = request->vn;
        echo->giveServiceData(data->clone());
        NetworkServiceSyntheticPacket packet;
        packet.trusted_src   = request->src;
        packet.route_vn      = ingress.event->getVN();
        packet.output_port   = static_cast<int>(request->dest);
        packet.request       = std::move(echo);
        if ( !host()->tryEnqueueNetworkServiceOutput(SERVICE_ID, packet) ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "probe could not enqueue its synthetic echo\n");
        }
    }
    bool hasScheduledWork() const override { return false; }
};

class ProbeEndpoint final : public Component
{
public:
    SST_ELI_REGISTER_COMPONENT(ProbeEndpoint, "merlin", "network_service_probe_endpoint",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Minimal network-service endpoint",
        COMPONENT_CATEGORY_NETWORK)
    SST_ELI_DOCUMENT_PARAMS({ "id", "Endpoint ID (zero or one)", "-1" })
    SST_ELI_DOCUMENT_PORTS()
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "networkIF", "Merlin network interface", "SST::Interfaces::SimpleNetwork" })

    ProbeEndpoint(ComponentId_t id, Params& params) : Component(id), id_(params.find<int>("id", -1))
    {
        network_ = loadUserSubComponent<SimpleNetwork>("networkIF", ComponentInfo::SHARE_NONE, 2);
        if ( network_ == nullptr || (id_ != 0 && id_ != 1) ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "invalid network-service probe endpoint\n");
        }
        deadline_ = configureSelfLink("deadline", "20ns",
            new Event::Handler<ProbeEndpoint, &ProbeEndpoint::deadline>(this));
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
    }

    void init(unsigned phase) override { network_->init(phase); }
    void complete(unsigned phase) override { network_->complete(phase); }
    void finish() override { network_->finish(); }

    void setup() override
    {
        network_->setup();
        SimpleNetwork::NetworkServiceCapability capability;
        if ( !network_->queryServiceCapability(SERVICE_ID, capability) ||
             !capability.isValidFor(SERVICE_ID) ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "probe service was not advertised\n");
        }
        network_->setNotifyOnReceive(
            new SimpleNetwork::Handler<ProbeEndpoint, &ProbeEndpoint::receive>(this));
        // Endpoint 0 sends on the processor-owned VN 0 and is held until the
        // release time; endpoint 1 sends on the unowned VN 1 and routes normally.
        const int vn = id_ == 0 ? 0 : 1;
        auto request = std::make_unique<SimpleNetwork::Request>(1 - id_, id_, 64, true, true);
        request->vn = vn;
        request->giveServiceData(new ProbeData(id_ == 0 ? Action::Busy : Action::Pass, id_));
        if ( !network_->send(request.get(), vn) ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "probe injection unexpectedly blocked\n");
        }
        request.release();
        deadline_->send(1, nullptr);
    }

private:
    bool receive(int vn)
    {
        while ( network_->requestToReceive(vn) ) {
            std::unique_ptr<SimpleNetwork::Request> request(network_->recv(vn));
            const auto* data = request == nullptr ? nullptr : request->inspectServiceDataAs<ProbeData>();
            const Action expected = id_ == 0 ? Action::Pass : Action::Busy;
            const uint32_t sequence = static_cast<uint32_t>(1 - id_);
            const int expected_vn = id_ == 0 ? 1 : 0;
            if ( vn != expected_vn || data == nullptr || data->action() != expected ||
                 data->sequence() != sequence || request->src != 1 - id_ || request->dest != id_ ||
                 request->vn != expected_vn || request->size_in_bits != 64 || received_ ) {
                getSimulationOutput().fatal(CALL_INFO, 1, "tagged PASS/BUSY packet changed in transit\n");
            }
            received_ = true;
            if ( id_ == 1 ) getSimulationOutput().output("Merlin network-service integration PASS\n");
            // Endpoint 1 receives only after the processor released its Busy hold.
            if ( id_ == 1 && getCurrentSimTimeNano() < BUSY_RELEASE_NS ) {
                getSimulationOutput().fatal(CALL_INFO, 1, "Busy head was delivered before release\n");
            }
            primaryComponentOKToEndSim();
        }
        return true;
    }

    void deadline(Event* event)
    {
        delete event;
        if ( !received_ ) getSimulationOutput().fatal(CALL_INFO, 1, "network-service probe timed out\n");
    }

    int id_ = -1;
    SimpleNetwork* network_ = nullptr;
    Link* deadline_ = nullptr;
    bool received_ = false;
};

class TrackingEvent final : public Event
{
public:
    TrackingEvent(int& clones, int& destructions, bool throws = false) :
        clones_(&clones), destructions_(&destructions), throws_(throws)
    {}
    ~TrackingEvent() override { ++*destructions_; }
    Event* clone() override
    {
        ++*clones_;
        if ( throws_ ) throw std::runtime_error("clone failed");
        return new TrackingEvent(*clones_, *destructions_);
    }

private:
    int* clones_;
    int* destructions_;
    bool throws_;
};

class FakePort final : public PortInterface
{
public:
    void setHead(internal_router_event* event) { head_[0] = event; }
    void recvCtrlEvent(CtrlRtrEvent*) override {}
    void sendCtrlEvent(CtrlRtrEvent*) override {}
    void send(internal_router_event*, int) override {}
    bool spaceToSend(int vc, int flits) override { return vc == 0 && flits > 0; }
    internal_router_event* recv(int) override { return nullptr; }
    internal_router_event** getVCHeads() override { return head_; }
    void reportIncomingEvent(internal_router_event*) override {}
    void initVCs(int, int*, internal_router_event**, int*, int*) override {}
    void sendUntimedData(Event* event) override { delete event; }
    Event* recvUntimedData() override { return nullptr; }
    bool decreaseLinkWidth() override { return false; }
    bool increaseLinkWidth() override { return false; }

private:
    internal_router_event* head_[1] = { nullptr };
};

std::unique_ptr<internal_router_event> arbitrationEvent(int flits = 1)
{
    auto* envelope = new RtrEvent(new SimpleNetwork::Request(0, 0, 64, true, true), 0, 0);
    require(envelope->setSyntheticTransportMetadata(flits, 0), "arbitration metadata failed");
    auto event = std::make_unique<internal_router_event>(envelope);
    event->setNextPort(0);
    event->setVC(0);
    return event;
}

void testOwnership()
{
    int clones = 0;
    int destructions = 0;
    {
        ExtendedRequest source(1, 0, 64, true, true, new TrackingEvent(clones, destructions));
        source.giveServiceData(new ProbeData(Action::Pass, 7));
        ExtendedRequest copy(source);
        ExtendedRequest assigned;
        assigned = source;
        ExtendedRequest moved(std::move(copy));
        require(clones == 2 && moved.inspectPayload() != source.inspectPayload() &&
                assigned.inspectPayload() != source.inspectPayload() &&
                moved.inspectServiceData() != source.inspectServiceData(),
            "ExtendedRequest copy/move ownership changed");
    }
    require(destructions == 3, "ExtendedRequest payload ownership leaked");

    clones = destructions = 0;
    {
        ExtendedRequest source(1, 0, 64, true, true, new TrackingEvent(clones, destructions, true));
        bool threw = false;
        try { ExtendedRequest copy(source); }
        catch ( const std::runtime_error& ) { threw = true; }
        require(threw && clones == 1 && destructions == 0 && source.inspectPayload() != nullptr,
            "failed ExtendedRequest copy damaged its source");
    }
    require(destructions == 1, "failed copy lost source ownership");

    clones = destructions = 0;
    {
        auto* request = new SimpleNetwork::Request(
            1, 0, 64, true, true, new TrackingEvent(clones, destructions));
        Event* payload = request->inspectPayload();
        ExtendedRequest wrapped = request;
        delete request;
        require(wrapped.inspectPayload() == payload && destructions == 0,
            "legacy Request wrapper did not transfer payload");
    }
    require(destructions == 1, "legacy wrapper payload was not singly owned");

    clones = destructions = 0;
    {
        internal_router_event original(new RtrEvent(new SimpleNetwork::Request(
            1, 0, 64, true, true, new TrackingEvent(clones, destructions)), 0, 0));
        internal_router_event copy(original);
        require(clones == 1 && copy.getEncapsulatedEvent() != original.getEncapsulatedEvent() &&
                copy.inspectRequest()->inspectPayload() != original.inspectRequest()->inspectPayload(),
            "router envelope copy aliased ownership");
    }
    require(destructions == 2, "router envelope copies leaked payloads");
}

void testLongPacketLRUFairness()
{
    constexpr int count = 13;
    std::array<FakePort, count> ports;
    std::array<std::unique_ptr<internal_router_event>, 2> events;
    std::vector<std::unique_ptr<NetworkServicePortXbarInput>> storage;
    std::array<XbarInput*, count> inputs {};
    for ( int index = 0; index < count; ++index ) {
        storage.emplace_back(new NetworkServicePortXbarInput(&ports[index]));
        inputs[index] = storage.back().get();
    }
    events[0] = arbitrationEvent(count);
    events[1] = arbitrationEvent(count);
    ports[0].setHead(events[0].get());
    ports[count - 1].setHead(events[1].get());
    std::array<PortInterface*, count - 1> outputs {};
    for ( int index = 0; index < count - 1; ++index ) outputs[index] = &ports[index];
    xbar_arb_lru arbiter;
    XbarArbitration& arbiter_api = arbiter;
    require(arbiter_api.setNetworkServiceInputs(count, count - 1, 1, std::vector<uint8_t>(1, 0)),
        "LRU rejected service input");
    int input_busy[count] = {};
    int output_busy[count - 1] = {};
    int progress[count];
    int physical_grants = 0;
    int synthetic_grants = 0;
    for ( int cycle = 0; cycle < 80; ++cycle ) {
        require(arbiter_api.arbitrateNetworkService(
                    inputs.data(), outputs.data(), input_busy, output_busy, progress),
            "LRU service arbitration failed");
        for ( int input = 0; input < count; ++input ) {
            require(progress[input] < 0 || input == 0 || input == count - 1,
                "empty LRU input received a grant");
        }
        physical_grants += progress[0] >= 0;
        synthetic_grants += progress[count - 1] >= 0;
        for ( int& busy : input_busy ) busy = std::max(0, busy - 1);
        for ( int& busy : output_busy ) busy = std::max(0, busy - 1);
    }
    require(physical_grants > 1 && synthetic_grants > 1,
        "13-cycle output occupancy phase-locked an LRU requester");
}

} // namespace SST::Merlin::Test

namespace SST::Collective::Test {

using namespace SST::Merlin;
using Request = SST::Interfaces::SimpleNetwork::Request;

template <class T>
std::vector<char> roundTrip(T& input, T& output)
{
    SST::Core::Serialization::serializer ser;
    ser.start_sizing(); SST_SER(input);
    std::vector<char> wire(ser.size());
    ser.start_packing(wire.data(), wire.size()); SST_SER(input);
    ser.start_unpacking(wire.data(), wire.size()); SST_SER(output);
    ser.finalize();
    return wire;
}

CollectiveParticipant participant()
{
    CollectiveParticipant value;
    value.route = { 1, 1 };
    value.physical_endpoint_id = 9;
    value.logical_participant_id = 100;
    value.reduce_vn = 0;
    value.result_vn = 1;
    return value;
}

CollectiveSubmission submission(uint64_t invocation, double& source, double& result)
{
    CollectiveSubmission value;
    value.invocation_id = invocation;
    value.signature = { CollectiveOperation::Sum, CollectiveDatatype::F64, 1 };
    value.source = { reinterpret_cast<const uint8_t*>(&source), sizeof(source) };
    value.result = { reinterpret_cast<uint8_t*>(&result), sizeof(result) };
    return value;
}

class Endpoint final : public StaticCollectiveEndpointBase
{
public:
    bool install(const CollectiveParticipant& value) { return installParticipant(value); }
    void setReady(bool value) { ready = value; }
    bool finish(uint64_t invocation, double result)
    {
        StaticCollectiveResult completed;
        completed.route = installedParticipant().route;
        completed.invocation_id = invocation;
        completed.signature = STATIC_COLLECTIVE_SIGNATURE_V1;
        completed.value.resize(sizeof(result));
        std::memcpy(completed.value.data(), &result, sizeof(result));
        return completeSuccess(completed);
    }
    bool ready = false;
    uint32_t commits = 0;
    uint64_t committed_invocation = 0;
    bool committed_value_valid = false;

protected:
    bool transportReady(const CollectiveSignatureV1&) const override { return ready; }
    void commitContribution(StaticCollectiveContribution&& contribution) noexcept override
    {
        ++commits;
        committed_invocation = contribution.invocation_id;
        committed_value_valid = contribution.valid() &&
            contribution.route == installedParticipant().route &&
            contribution.signature == STATIC_COLLECTIVE_SIGNATURE_V1;
    }
};

class Sink final : public CollectiveCompletionSink, public CollectiveReadySink
{
public:
    void complete(uint64_t invocation_id, CollectiveCompletionStatus status) override
    {
        ++completions;
        last_invocation = invocation_id;
        visible = observed == nullptr || *observed == expected;
        status_ok = status == CollectiveCompletionStatus::Success;
        if ( complete_next != nullptr ) {
            CollectiveSubmission* next = std::exchange(complete_next, nullptr);
            complete_submit = endpoint->trySubmitCollective(*next);
        }
    }
    void ready() override
    {
        ++readies;
        if ( ready_next != nullptr ) {
            CollectiveSubmission* next = std::exchange(ready_next, nullptr);
            ready_submit = endpoint->trySubmitCollective(*next);
        }
    }
    Endpoint* endpoint = nullptr;
    CollectiveSubmission* ready_next = nullptr;
    CollectiveSubmission* complete_next = nullptr;
    const double* observed = nullptr;
    double expected = 0;
    uint64_t last_invocation = 0;
    uint32_t completions = 0;
    uint32_t readies = 0;
    CollectiveSubmitResult ready_submit = static_cast<CollectiveSubmitResult>(0);
    CollectiveSubmitResult complete_submit = static_cast<CollectiveSubmitResult>(0);
    bool visible = false;
    bool status_ok = false;
};

void testCollectiveSignature()
{
    const CollectiveSignatureV1 scalar {
        CollectiveOperation::Sum, CollectiveDatatype::F64, 1 };
    const CollectiveSignatureV1 vector {
        CollectiveOperation::Max, CollectiveDatatype::I32, 128 };
    const CollectiveSignatureV1 overflow { CollectiveOperation::Min, CollectiveDatatype::U64,
        std::numeric_limits<uint64_t>::max() / 8 + 1 };

    SST::Merlin::Test::require(scalar.valid() && scalar.payloadBytes() == 8 &&
            vector.valid() && vector.payloadBytes() == 512 && !overflow.valid() &&
            !overflow.payloadBytes() && collectiveDatatypeBytes(CollectiveDatatype::F32) == 4,
        "collective signature validation or sizing changed");
}

void testStaticEndpoint()
{
    Endpoint endpoint;
    Sink sink;
    sink.endpoint = &endpoint;
    SST::Merlin::Test::require(endpoint.participant() == nullptr && !endpoint.bind(sink, sink),
        "static endpoint published or bound before a participant was installed");
    CollectiveParticipant same_vns = participant();
    same_vns.result_vn = same_vns.reduce_vn;
    SST::Merlin::Test::require(!endpoint.install(same_vns), "identical service VNs were installed");
    SST::Merlin::Test::require(endpoint.install(participant()), "static endpoint install failed");
    const CollectiveParticipant* installed = endpoint.participant();
    SST::Merlin::Test::require(installed != nullptr && *installed == participant(),
        "static endpoint participant missing or changed");
    SST::Merlin::Test::require(!endpoint.install(participant()), "participant was installed twice");
    SST::Merlin::Test::require(endpoint.bind(sink, sink) && !endpoint.bind(sink, sink),
        "static endpoint binding is not exactly once");

    double unsupported_source = 2.0, unsupported_result = -1.0;
    CollectiveSubmission unsupported = submission(6, unsupported_source, unsupported_result);
    unsupported.signature = {
        CollectiveOperation::Max, CollectiveDatatype::I32, 128 };
    SST::Merlin::Test::require(
        endpoint.supportsCollective(unsupported.signature) == false &&
            endpoint.trySubmitCollective(unsupported) == CollectiveSubmitResult::Unsupported,
        "unsupported collective signature was not reported as Unsupported");

    CollectiveSubmission malformed = submission(6, unsupported_source, unsupported_result);
    malformed.signature.element_count = 0;
    SST::Merlin::Test::require(
        endpoint.trySubmitCollective(malformed) == CollectiveSubmitResult::Invalid,
        "malformed collective signature was not rejected");

    double source1 = 2.0, result1 = -1.0;
    CollectiveSubmission first = submission(7, source1, result1);
    SST::Merlin::Test::require(endpoint.trySubmitCollective(first) == CollectiveSubmitResult::Retry,
        "unready static endpoint accepted a submission");
    endpoint.setReady(true);
    sink.ready_next = &first;
    endpoint.requestCollectiveReady(first.signature);
    SST::Merlin::Test::require(sink.readies == 1 && sink.ready_submit == CollectiveSubmitResult::Accepted &&
            endpoint.commits == 1 && endpoint.committed_value_valid,
        "ready callback was not reentrant after arming");

    double source2 = 3.0, result2 = -1.0;
    CollectiveSubmission second = submission(8, source2, result2);
    SST::Merlin::Test::require(endpoint.trySubmitCollective(second) == CollectiveSubmitResult::Retry,
        "active static endpoint accepted a second invocation");
    sink.observed = &result1;
    sink.expected = 9.0;
    sink.complete_next = &second;
    SST::Merlin::Test::require(endpoint.finish(7, 9.0) && result1 == 9.0 && sink.visible && sink.status_ok &&
            sink.last_invocation == 7 && sink.complete_submit == CollectiveSubmitResult::Accepted &&
            endpoint.commits == 2 && endpoint.committed_invocation == 8,
        "completion did not publish before reentrant submission");
    SST::Merlin::Test::require(endpoint.finish(8, 10.0) && result2 == 10.0 && endpoint.quiescent(),
        "reentrant static invocation did not complete");

    double replay_result = -1.0;
    CollectiveSubmission replay = submission(7, source1, replay_result);
    SST::Merlin::Test::require(endpoint.trySubmitCollective(replay) == CollectiveSubmitResult::Invalid,
        "retired static invocation reopened");
}

RouteIdV1 route() { return { 1, 1 }; }

void testServiceDataContract()
{
    const std::vector<uint8_t> bytes { 1, 2, 3, 4, 5, 6, 7, 8 };
    CollectiveServiceData original(route(), 17, CollectiveDirection::Contribution,
        STATIC_COLLECTIVE_SIGNATURE_V1, 0, bytes);
    CollectiveServiceData decoded;
    std::vector<char> wire = roundTrip(original, decoded);
    // route(16) invocation(8) direction(1) signature(10) chunk(4) value(8 + 8)
    SST::Merlin::Test::require(wire.size() == 55 && decoded.route == original.route &&
            decoded.invocation_id == 17 && decoded.direction == original.direction &&
            decoded.signature == STATIC_COLLECTIVE_SIGNATURE_V1 && decoded.chunk_index == 0 &&
            decoded.value == bytes,
        "collective sidecar wire layout or round-trip changed");

    std::unique_ptr<CollectiveServiceData> clone(original.clone());
    original.value[0] ^= 0xff;
    SST::Merlin::Test::require(clone->value == bytes && clone->value != original.value,
        "collective sidecar clone aliased its source");

    CollectiveServiceData wider(route(), 18, CollectiveDirection::Contribution,
        { CollectiveOperation::Max, CollectiveDatatype::I32, 128 }, 0, std::vector<uint8_t>(512));
    CollectiveServiceData wider_decoded;
    roundTrip(wider, wider_decoded);
    SST::Merlin::Test::require(wider.validateIntrinsic() == DescriptorValidation::Valid &&
            wider_decoded.value.size() == 512 && wider_decoded.signature == wider.signature &&
            CollectiveServiceData::modeledRequestBits(wider.signature) == (90 + 512) * 8 &&
            !wider.validFor(route(), CollectiveDirection::Contribution, STATIC_COLLECTIVE_REQUEST_BITS),
        "signature-described sidecar does not describe a wider payload");
    CollectiveServiceData short_value(route(), 19, CollectiveDirection::Contribution,
        STATIC_COLLECTIVE_SIGNATURE_V1, 0, std::vector<uint8_t>(4));
    SST::Merlin::Test::require(short_value.validateIntrinsic() == DescriptorValidation::InvalidValue,
        "payload shorter than its signature was accepted");

    wire[3 * sizeof(uint64_t)] = 0;
    bool rejected = false;
    try {
        CollectiveServiceData malformed;
        SST::Core::Serialization::serializer ser;
        ser.start_unpacking(wire.data(), wire.size()); SST_SER(malformed);
    }
    catch ( const std::runtime_error& ) { rejected = true; }
    SST::Merlin::Test::require(rejected, "malformed collective direction was deserialized");

    Request request(7, 9, STATIC_COLLECTIVE_REQUEST_BITS, true, true);
    request.vn = 1;
    request.giveServiceData(new CollectiveServiceData(decoded));
    std::unique_ptr<Request> request_clone(request.clone());
    Request request_decoded;
    roundTrip(request, request_decoded);
    const auto* cloned_data = request_clone->inspectServiceDataAs<CollectiveServiceData>();
    const auto* decoded_data = request_decoded.inspectServiceDataAs<CollectiveServiceData>();
    SST::Merlin::Test::require(cloned_data != nullptr && decoded_data != nullptr &&
            cloned_data != request.inspectServiceData() && cloned_data->value == bytes &&
            decoded_data->value == bytes && request_decoded.dest == 7 && request_decoded.vn == 1,
        "Request clone or serialization lost the collective sidecar");

    CollectiveParticipant identity = participant();
    CollectiveParticipant identity_decoded;
    roundTrip(identity, identity_decoded);
    SST::Merlin::Test::require(identity_decoded == identity && identity_decoded.valid(),
        "collective participant identity did not round-trip");
}

class ContractTest final : public Component
{
public:
    SST_ELI_REGISTER_COMPONENT(ContractTest, "merlin", "collective_contract_test",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Compact Merlin collective contract test",
        COMPONENT_CATEGORY_UNCATEGORIZED)
    SST_ELI_DOCUMENT_PARAMS()

    ContractTest(ComponentId_t id, Params&) : Component(id)
    {
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
        try {
            SST::Merlin::Test::testOwnership();
            SST::Merlin::Test::testLongPacketLRUFairness();
            testCollectiveSignature();
            testStaticEndpoint();
            testServiceDataContract();
        }
        catch ( const std::exception& error ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "Merlin collective contract FAIL: %s\n", error.what());
        }
        getSimulationOutput().output("Merlin collective contract PASS\n");
        primaryComponentOKToEndSim();
    }
};

} // namespace SST::Collective::Test
