// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "sst_config.h"

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

namespace SST::Collective::Test {

void require(bool condition, const char* message)
{
    if ( !condition ) throw std::runtime_error(message);
}
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

    require(scalar.valid() && scalar.payloadBytes() == 8 &&
            vector.valid() && vector.payloadBytes() == 512 && !overflow.valid() &&
            !overflow.payloadBytes() && collectiveDatatypeBytes(CollectiveDatatype::F32) == 4,
        "collective signature validation or sizing changed");
}

void testStaticEndpoint()
{
    Endpoint endpoint;
    Sink sink;
    sink.endpoint = &endpoint;
    require(endpoint.participant() == nullptr && !endpoint.bind(sink, sink),
        "static endpoint published or bound before a participant was installed");
    CollectiveParticipant same_vns = participant();
    same_vns.result_vn = same_vns.reduce_vn;
    require(!endpoint.install(same_vns), "identical service VNs were installed");
    require(endpoint.install(participant()), "static endpoint install failed");
    const CollectiveParticipant* installed = endpoint.participant();
    require(installed != nullptr && *installed == participant(),
        "static endpoint participant missing or changed");
    require(!endpoint.install(participant()), "participant was installed twice");
    require(endpoint.bind(sink, sink) && !endpoint.bind(sink, sink),
        "static endpoint binding is not exactly once");

    double unsupported_source = 2.0, unsupported_result = -1.0;
    CollectiveSubmission unsupported = submission(6, unsupported_source, unsupported_result);
    unsupported.signature = {
        CollectiveOperation::Max, CollectiveDatatype::I32, 128 };
    require(
        endpoint.supportsCollective(unsupported.signature) == false &&
            endpoint.trySubmitCollective(unsupported) == CollectiveSubmitResult::Unsupported,
        "unsupported collective signature was not reported as Unsupported");

    CollectiveSubmission malformed = submission(6, unsupported_source, unsupported_result);
    malformed.signature.element_count = 0;
    require(
        endpoint.trySubmitCollective(malformed) == CollectiveSubmitResult::Invalid,
        "malformed collective signature was not rejected");

    double source1 = 2.0, result1 = -1.0;
    CollectiveSubmission first = submission(7, source1, result1);
    require(endpoint.trySubmitCollective(first) == CollectiveSubmitResult::Retry,
        "unready static endpoint accepted a submission");
    endpoint.setReady(true);
    sink.ready_next = &first;
    endpoint.requestCollectiveReady(first.signature);
    require(sink.readies == 1 && sink.ready_submit == CollectiveSubmitResult::Accepted &&
            endpoint.commits == 1 && endpoint.committed_value_valid,
        "ready callback was not reentrant after arming");

    double source2 = 3.0, result2 = -1.0;
    CollectiveSubmission second = submission(8, source2, result2);
    require(endpoint.trySubmitCollective(second) == CollectiveSubmitResult::Retry,
        "active static endpoint accepted a second invocation");
    sink.observed = &result1;
    sink.expected = 9.0;
    sink.complete_next = &second;
    require(endpoint.finish(7, 9.0) && result1 == 9.0 && sink.visible && sink.status_ok &&
            sink.last_invocation == 7 && sink.complete_submit == CollectiveSubmitResult::Accepted &&
            endpoint.commits == 2 && endpoint.committed_invocation == 8,
        "completion did not publish before reentrant submission");
    require(endpoint.finish(8, 10.0) && result2 == 10.0 && endpoint.quiescent(),
        "reentrant static invocation did not complete");

    double replay_result = -1.0;
    CollectiveSubmission replay = submission(7, source1, replay_result);
    require(endpoint.trySubmitCollective(replay) == CollectiveSubmitResult::Invalid,
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
    require(wire.size() == 55 && decoded.route == original.route &&
            decoded.invocation_id == 17 && decoded.direction == original.direction &&
            decoded.signature == STATIC_COLLECTIVE_SIGNATURE_V1 && decoded.chunk_index == 0 &&
            decoded.value == bytes,
        "collective sidecar wire layout or round-trip changed");

    std::unique_ptr<CollectiveServiceData> clone(original.clone());
    original.value[0] ^= 0xff;
    require(clone->value == bytes && clone->value != original.value,
        "collective sidecar clone aliased its source");

    CollectiveServiceData wider(route(), 18, CollectiveDirection::Contribution,
        { CollectiveOperation::Max, CollectiveDatatype::I32, 128 }, 0, std::vector<uint8_t>(512));
    CollectiveServiceData wider_decoded;
    roundTrip(wider, wider_decoded);
    require(wider.validateIntrinsic() == DescriptorValidation::Valid &&
            wider_decoded.value.size() == 512 && wider_decoded.signature == wider.signature &&
            CollectiveServiceData::modeledRequestBits(wider.signature) == (90 + 512) * 8 &&
            !wider.validFor(route(), CollectiveDirection::Contribution, STATIC_COLLECTIVE_REQUEST_BITS),
        "signature-described sidecar does not describe a wider payload");
    CollectiveServiceData short_value(route(), 19, CollectiveDirection::Contribution,
        STATIC_COLLECTIVE_SIGNATURE_V1, 0, std::vector<uint8_t>(4));
    require(short_value.validateIntrinsic() == DescriptorValidation::InvalidValue,
        "payload shorter than its signature was accepted");

    wire[3 * sizeof(uint64_t)] = 0;
    bool rejected = false;
    try {
        CollectiveServiceData malformed;
        SST::Core::Serialization::serializer ser;
        ser.start_unpacking(wire.data(), wire.size()); SST_SER(malformed);
    }
    catch ( const std::runtime_error& ) { rejected = true; }
    require(rejected, "malformed collective direction was deserialized");

    Request request(7, 9, STATIC_COLLECTIVE_REQUEST_BITS, true, true);
    request.vn = 1;
    request.giveServiceData(new CollectiveServiceData(decoded));
    std::unique_ptr<Request> request_clone(request.clone());
    Request request_decoded;
    roundTrip(request, request_decoded);
    const auto* cloned_data = request_clone->inspectServiceDataAs<CollectiveServiceData>();
    const auto* decoded_data = request_decoded.inspectServiceDataAs<CollectiveServiceData>();
    require(cloned_data != nullptr && decoded_data != nullptr &&
            cloned_data != request.inspectServiceData() && cloned_data->value == bytes &&
            decoded_data->value == bytes && request_decoded.dest == 7 && request_decoded.vn == 1,
        "Request clone or serialization lost the collective sidecar");

    CollectiveParticipant identity = participant();
    CollectiveParticipant identity_decoded;
    roundTrip(identity, identity_decoded);
    require(identity_decoded == identity && identity_decoded.valid(),
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
