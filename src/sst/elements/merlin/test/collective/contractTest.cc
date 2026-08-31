// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "sst_config.h"

#include <sst/elements/merlin/services/collective/collectiveEndpoint.h>
#include <sst/elements/merlin/services/collective/collectiveServiceData.h>

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/output.h>

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace SST::Collective::Test {

using Request = SST::Interfaces::SimpleNetwork::Request;

void require(bool condition, const char* message)
{
    if ( !condition ) throw std::runtime_error(message);
}

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

RouteIdV1 route() { return { 1, 1 }; }

AcceptedParticipantHandle participant()
{
    AcceptedParticipantHandle value;
    value.route = route();
    value.physical_route = { 0, 1 };
    value.route_kind = CollectiveRouteKind::FabricTree;
    value.data_mode = CollectiveDataMode::Functional;
    value.physical_endpoint_id = 9;
    value.local_participant_count = 1;
    value.logical_participant_id = 100;
    value.binding = { 1, 0, 1 };
    value.accepted_invocation_quota = 1;
    value.submission_window = 1;
    value.fabric.emplace(FabricParticipantRouteV1 { 0, 1, 10 });
    return value;
}

CollectivePending pending(const AcceptedParticipantHandle& owner, uint64_t invocation,
    uint64_t request, double& source, double& result)
{
    CollectivePending value;
    value.participant = owner;
    value.invocation_id = invocation;
    value.signature = STATIC_COLLECTIVE_SIGNATURE_V1;
    value.source = { reinterpret_cast<const uint8_t*>(&source), sizeof(source) };
    value.result = { reinterpret_cast<uint8_t*>(&result), sizeof(result) };
    value.completion = CollectiveCompletionToken(0, request, 1);
    return value;
}

class Endpoint final : public CollectiveEndpoint
{
public:
    bool supportsCollective(const CollectiveSignatureV1& signature) const override
    {
        return signature.valid() && signature == STATIC_COLLECTIVE_SIGNATURE_V1;
    }

    bool bindParticipant(const AcceptedParticipantHandle& value, CollectiveCompletionSink& completion,
        CollectiveReadySink& ready) override
    {
        if ( !value.valid() || participant_ != nullptr ) return false;
        participant_ = &value;
        completion_ = &completion;
        ready_ = &ready;
        return true;
    }

    CollectiveSubmitResult trySubmitCollective(CollectivePending& value) override
    {
        if ( !value.readyForSubmit() || participant_ == nullptr ||
             value.participant.route != participant_->route || !value.signature.valid() )
            return CollectiveSubmitResult::Invalid;
        if ( !supportsCollective(value.signature) ) return CollectiveSubmitResult::Unsupported;
        if ( retry_ || token_ ) return CollectiveSubmitResult::Retry;
        result_ = value.result;
        invocation_ = value.invocation_id;
        token_.emplace(value.consumeAfterAcceptance());
        return CollectiveSubmitResult::Accepted;
    }

    void requestCollectiveReady(const AcceptedParticipantHandle& value,
        const CollectiveSignatureV1& signature) override
    {
        if ( ready_ != nullptr && value.route == participant_->route && supportsCollective(signature) )
            ready_->ready(value);
    }

    void setRetry(bool value) { retry_ = value; }
    bool finish(uint64_t invocation, double value)
    {
        if ( !token_ || invocation != invocation_ || result_.bytes != sizeof(value) ) return false;
        std::memcpy(result_.data, &value, sizeof(value));
        completion_->complete(std::move(*token_), CollectiveCompletionStatus::Success);
        token_.reset();
        return true;
    }

private:
    const AcceptedParticipantHandle* participant_ = nullptr;
    CollectiveCompletionSink* completion_ = nullptr;
    CollectiveReadySink* ready_ = nullptr;
    std::optional<CollectiveCompletionToken> token_;
    MutableBufferView result_;
    uint64_t invocation_ = 0;
    bool retry_ = false;
};

class Sink final : public CollectiveCompletionSink, public CollectiveReadySink
{
public:
    void complete(CollectiveCompletionToken&& token, CollectiveCompletionStatus status) override
    {
        ++completions;
        request = token.nativeRequestId();
        visible = observed != nullptr && *observed == expected;
        success = status == CollectiveCompletionStatus::Success;
    }
    void ready(const AcceptedParticipantHandle&) override { ++readies; }
    const double* observed = nullptr;
    double expected = 0;
    uint64_t request = 0;
    uint32_t completions = 0;
    uint32_t readies = 0;
    bool visible = false;
    bool success = false;
};

void testCollectiveSignature()
{
    const CollectiveSignatureV1 scalar = STATIC_COLLECTIVE_SIGNATURE_V1;
    const CollectiveSignatureV1 vector {
        CollectiveOperation::Max, CollectiveDatatype::I32, 128 };
    const CollectiveSignatureV1 overflow { CollectiveOperation::Min, CollectiveDatatype::U64,
        std::numeric_limits<uint64_t>::max() / 8 + 1 };

    require(scalar.valid() && scalar.payloadBytes() == 8 &&
            vector.valid() && vector.payloadBytes() == 512 && !overflow.valid() &&
            !overflow.payloadBytes() && collectiveDatatypeBytes(CollectiveDatatype::F32) == 4,
        "collective signature validation or sizing changed");
}

void testEndpoint()
{
    const AcceptedParticipantHandle owner = participant();
    Endpoint endpoint;
    Sink sink;
    require(endpoint.bindParticipant(owner, sink, sink), "collective endpoint binding failed");
    double source = 2.0, result = -1.0;

    endpoint.setRetry(true);
    CollectivePending retry = pending(owner, 7, 41, source, result);
    require(endpoint.trySubmitCollective(retry) == CollectiveSubmitResult::Retry && retry.readyForSubmit(),
        "Retry consumed collective ownership");
    endpoint.requestCollectiveReady(owner, retry.signature);
    require(sink.readies == 1, "collective ready callback was not delivered");

    CollectivePending unsupported = pending(owner, 7, 42, source, result);
    unsupported.signature.operation = CollectiveOperation::Min;
    require(endpoint.trySubmitCollective(unsupported) == CollectiveSubmitResult::Unsupported &&
            unsupported.readyForSubmit(), "Unsupported consumed collective ownership");

    endpoint.setRetry(false);
    CollectivePending accepted = pending(owner, 7, 43, source, result);
    require(endpoint.trySubmitCollective(accepted) == CollectiveSubmitResult::Accepted &&
            !accepted.readyForSubmit(), "Accepted did not consume collective ownership");
    sink.observed = &result;
    sink.expected = 9.0;
    require(endpoint.finish(7, 9.0) && !endpoint.finish(7, 10.0) && sink.completions == 1 &&
            sink.request == 43 && sink.visible && sink.success,
        "collective completion was not visible and exactly once");
}

void testServiceDataContract()
{
    std::array<uint8_t, CollectiveServiceData::VALUE_BYTES> bytes { 1, 2, 3, 4, 5, 6, 7, 8 };
    CollectiveServiceData original(route(), 17, CollectiveDirection::Contribution, bytes);
    CollectiveServiceData decoded;
    std::vector<char> wire = roundTrip(original, decoded);
    require(wire.size() == 33 && decoded.route == original.route && decoded.invocation_id == 17 &&
            decoded.direction == original.direction && decoded.value == bytes,
        "collective sidecar wire layout or round-trip changed");

    std::unique_ptr<CollectiveServiceData> clone(original.clone());
    original.value[0] ^= 0xff;
    require(clone->value == bytes && clone->value != original.value,
        "collective sidecar clone aliased its source");

    wire[3 * sizeof(uint64_t)] = 0;
    bool rejected = false;
    try {
        CollectiveServiceData malformed;
        SST::Core::Serialization::serializer ser;
        ser.start_unpacking(wire.data(), wire.size()); SST_SER(malformed);
    }
    catch ( const std::runtime_error& ) { rejected = true; }
    require(rejected, "malformed collective direction was deserialized");

    Request request(7, 9, CollectiveServiceData::MODELED_REQUEST_BITS, true, true);
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
            testEndpoint();
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
