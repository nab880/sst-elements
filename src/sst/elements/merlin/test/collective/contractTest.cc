// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "sst_config.h"

#include "contractTest.h"

#include <sst/elements/merlin/services/collective/collectiveEndpoint.h>
#include <sst/elements/merlin/services/collective/collectiveServiceData.h>

#include <sst/core/output.h>

#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace SST::Collective {
namespace {

void require(bool condition, const char* message)
{
    if ( !condition ) throw std::runtime_error(message);
}

template <class T>
void roundTrip(T& input, T& output)
{
    SST::Core::Serialization::serializer ser;
    ser.start_sizing();
    SST_SER(input);
    std::vector<char> buffer(ser.size());
    ser.start_packing(buffer.data(), buffer.size());
    SST_SER(input);
    ser.start_unpacking(buffer.data(), buffer.size());
    SST_SER(output);
    ser.finalize();
}

template <class T>
std::vector<char> pack(T& input)
{
    SST::Core::Serialization::serializer ser;
    ser.start_sizing();
    SST_SER(input);
    std::vector<char> buffer(ser.size());
    ser.start_packing(buffer.data(), buffer.size());
    SST_SER(input);
    return buffer;
}

constexpr RouteIdV1 TEST_ROUTE { 0x0102030405060708ull, 0x2122232425262728ull };
constexpr uint64_t  TEST_INVOCATION = 0x1112131415161718ull;

CollectiveServiceData makeServiceData(double value = 7.0)
{
    std::array<uint8_t, CollectiveServiceData::VALUE_BYTES> bytes {};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return CollectiveServiceData(
        TEST_ROUTE, TEST_INVOCATION, CollectiveDirection::Contribution, bytes);
}

AcceptedParticipantHandle makeParticipant(uint32_t slot = 0)
{
    AcceptedParticipantHandle participant;
    participant.route                       = TEST_ROUTE;
    participant.physical_route              = { 3, 1 };
    participant.route_kind                  = CollectiveRouteKind::FabricTree;
    participant.data_mode                   = CollectiveDataMode::Functional;
    participant.physical_endpoint_id        = 9;
    participant.local_participant_slot      = slot;
    participant.local_participant_count     = 1;
    participant.logical_participant_id      = 100 + slot;
    participant.binding                     = { 44, slot, 1 };
    participant.accepted_invocation_quota   = 1;
    participant.submission_window            = 1;
    participant.fabric.emplace();
    participant.fabric->endpoint_reduce_vn  = 1;
    participant.fabric->endpoint_result_vn  = 2;
    participant.fabric->injection_dest_nid  = 7;
    return participant;
}

void testDescriptor()
{
    CollectiveServiceData data = makeServiceData();
    require(data.validateIntrinsic() == DescriptorValidation::Valid, "valid descriptor rejected");
    require(data.validFor(TEST_ROUTE, CollectiveDirection::Contribution,
                CollectiveServiceData::MODELED_REQUEST_BITS),
        "valid fixed request rejected");
    require(data.serviceID() == COLLECTIVE_SERVICE_ID && data.dataToken() == COLLECTIVE_DATA_TOKEN &&
                data.schemaVersion() == COLLECTIVE_SERVICE_SCHEMA_V1,
        "service identity changed");

    CollectiveServiceData decoded;
    roundTrip(data, decoded);
    require(decoded.route == data.route && decoded.invocation_id == data.invocation_id &&
                decoded.direction == data.direction && decoded.value == data.value,
        "direct serialization changed service data");
    std::vector<char> packed = pack(data);
    require(packed.size() == 33,
        "fixed service-data serialization is not route/id/direction/value only");

    packed[3 * sizeof(uint64_t)] = 0;
    bool malformed_rejected = false;
    try {
        CollectiveServiceData malformed;
        SST::Core::Serialization::serializer ser;
        ser.start_unpacking(packed.data(), packed.size());
        SST_SER(malformed);
    }
    catch ( const std::runtime_error& ) {
        malformed_rejected = true;
    }
    require(malformed_rejected, "invalid serialized direction accepted");

    std::unique_ptr<CollectiveServiceData> data_clone(data.clone());
    const uint8_t cloned_first_byte = data_clone->value[0];
    data.value[0] ^= 0xff;
    require(data_clone.get() != &data && data_clone->value[0] == cloned_first_byte &&
                data_clone->value != data.value,
        "service-data clone aliased its source value");
    data.value[0] ^= 0xff;

    SimpleNetwork::Request request(
        7, 9, CollectiveServiceData::MODELED_REQUEST_BITS, true, true);
    request.vn = 1;
    request.giveServiceData(new CollectiveServiceData(data));
    std::unique_ptr<SimpleNetwork::Request> request_clone(request.clone());
    require(request_clone->getServiceID() == COLLECTIVE_SERVICE_ID &&
                request_clone->inspectServiceData() != request.inspectServiceData() &&
                request_clone->inspectServiceDataAs<CollectiveServiceData>() != nullptr,
        "Request clone sliced or aliased collective sidecar");

    auto* serialized_input  = new SimpleNetwork::Request(request);
    SimpleNetwork::Request* serialized_output = nullptr;
    roundTrip(serialized_input, serialized_output);
    require(serialized_output != nullptr &&
                serialized_output->inspectServiceDataAs<CollectiveServiceData>() != nullptr &&
                serialized_output->getServiceID() == COLLECTIVE_SERVICE_ID,
        "polymorphic Request serialization lost collective sidecar");
    delete serialized_input;
    delete serialized_output;

    CollectiveServiceData invalid = data;
    invalid.route.job_namespace   = 0;
    require(invalid.validateIntrinsic() == DescriptorValidation::InvalidRoute,
        "invalid route accepted");
    invalid               = data;
    invalid.invocation_id = 0;
    require(invalid.validateIntrinsic() == DescriptorValidation::InvalidInvocationId,
        "zero invocation ID accepted");
    invalid           = data;
    invalid.direction = static_cast<CollectiveDirection>(0);
    require(invalid.validateIntrinsic() == DescriptorValidation::InvalidDirection,
        "invalid direction accepted");

    require(!data.validFor({ TEST_ROUTE.job_namespace, TEST_ROUTE.route_id + 1 },
                CollectiveDirection::Contribution, CollectiveServiceData::MODELED_REQUEST_BITS),
        "wrong route accepted");
    require(!data.validFor(TEST_ROUTE, CollectiveDirection::Result,
                CollectiveServiceData::MODELED_REQUEST_BITS),
        "wrong direction accepted");
    require(!data.validFor(TEST_ROUTE, CollectiveDirection::Contribution,
                CollectiveServiceData::MODELED_REQUEST_BITS - 1),
        "undersized request accepted");
    require(!data.validFor(TEST_ROUTE, CollectiveDirection::Contribution,
                CollectiveServiceData::MODELED_REQUEST_BITS + 1),
        "oversized request accepted");
}

class CompletionRecorder final : public CollectiveCompletionSink
{
public:
    void complete(CollectiveCompletionToken&& token, CollectiveCompletionStatus status) override
    {
        ++count;
        last_request = token.nativeRequestId();
        last_status  = status;
        if ( observed_result != nullptr ) result_was_visible = *observed_result == expected_result;
    }

    uint32_t                   count = 0;
    uint64_t                   last_request = 0;
    CollectiveCompletionStatus last_status = static_cast<CollectiveCompletionStatus>(0);
    const double*              observed_result = nullptr;
    double                     expected_result = 0;
    bool                       result_was_visible = false;
};

class ReadyRecorder final : public CollectiveReadySink
{
public:
    void ready(const AcceptedParticipantHandle&) override { ++count; }
    uint32_t count = 0;
};

class FakeEndpoint final : public CollectiveEndpoint
{
public:
    bool bindParticipant(const AcceptedParticipantHandle& participant, CollectiveCompletionSink& completion,
        CollectiveReadySink& ready) override
    {
        if ( !participant.valid() || completion_ != nullptr ) return false;
        participant_ = &participant;
        completion_  = &completion;
        ready_       = &ready;
        return true;
    }

    CollectiveSubmitResult trySubmitCollective(CollectivePending& pending) override
    {
        if ( !pending.readyForSubmit() || !pending.participant.valid() || participant_ == nullptr ||
             pending.participant.route != participant_->route || pending.element_count == 0 ) {
            return CollectiveSubmitResult::Invalid;
        }
        if ( pending.operation != CollectiveOperation::Sum || pending.datatype != CollectiveDatatype::F64 ) {
            return CollectiveSubmitResult::Unsupported;
        }
        if ( retry_ ) return CollectiveSubmitResult::Retry;
        if ( active_ ) return CollectiveSubmitResult::Retry;

        token_.emplace(pending.consumeAfterAcceptance());
        result_ = pending.result;
        active_ = true;
        return CollectiveSubmitResult::Accepted;
    }

    void requestCollectiveReady(const AcceptedParticipantHandle& participant) override
    {
        if ( ready_ != nullptr && participant.route == participant_->route ) ready_->ready(participant);
    }

    void setRetry(bool value) { retry_ = value; }

    bool finish(double value)
    {
        if ( !active_ || !token_ || result_.bytes != sizeof(value) ) return false;
        std::memcpy(result_.data, &value, sizeof(value));
        completion_->complete(std::move(*token_), CollectiveCompletionStatus::Success);
        token_.reset();
        active_ = false;
        return true;
    }

private:
    const AcceptedParticipantHandle* participant_ = nullptr;
    CollectiveCompletionSink*        completion_ = nullptr;
    CollectiveReadySink*             ready_ = nullptr;
    std::optional<CollectiveCompletionToken> token_;
    MutableBufferView                result_;
    bool                             retry_ = false;
    bool                             active_ = false;
};

CollectivePending makePending(const AcceptedParticipantHandle& participant, double* source, double* result,
    CollectiveOperation operation, uint64_t request_id)
{
    CollectivePending pending;
    pending.participant   = participant;
    pending.invocation_id = 5;
    pending.operation     = operation;
    pending.datatype      = CollectiveDatatype::F64;
    pending.element_count = 1;
    pending.source        = { reinterpret_cast<const uint8_t*>(source), sizeof(*source) };
    pending.result        = { reinterpret_cast<uint8_t*>(result), sizeof(*result) };
    pending.completion    = CollectiveCompletionToken(0, request_id, 1);
    return pending;
}

void requirePendingUnchanged(const CollectivePending& pending, uint64_t request_id, const double* source,
    const double* result, CollectiveOperation operation)
{
    require(pending.state == CollectivePendingState::Ready && pending.completion.valid() &&
                pending.completion.nativeRequestId() == request_id && pending.source.data ==
                    reinterpret_cast<const uint8_t*>(source) &&
                pending.result.data == reinterpret_cast<const uint8_t*>(result) && pending.operation == operation,
        "non-accepted outcome moved or changed pending state");
}

void testEndpoint()
{
    const AcceptedParticipantHandle participant = makeParticipant();
    require(participant.valid(), "valid participant handle rejected");
    CompletionRecorder completion;
    ReadyRecorder      ready;
    FakeEndpoint       endpoint;
    require(endpoint.bindParticipant(participant, completion, ready), "participant binding failed");

    double source = 2.0;
    double result = -1.0;
    endpoint.setRetry(true);
    CollectivePending retry = makePending(participant, &source, &result, CollectiveOperation::Sum, 10);
    require(endpoint.trySubmitCollective(retry) == CollectiveSubmitResult::Retry, "Retry outcome missing");
    requirePendingUnchanged(retry, 10, &source, &result, CollectiveOperation::Sum);
    endpoint.requestCollectiveReady(participant);
    require(ready.count == 1, "ready notification did not use pre-registered sink");

    CollectivePending unsupported = makePending(participant, &source, &result, CollectiveOperation::Min, 11);
    require(endpoint.trySubmitCollective(unsupported) == CollectiveSubmitResult::Unsupported,
        "Unsupported outcome missing");
    requirePendingUnchanged(unsupported, 11, &source, &result, CollectiveOperation::Min);

    AcceptedParticipantHandle bad_participant = participant;
    bad_participant.schema_version             = 0;
    CollectivePending invalid = makePending(bad_participant, &source, &result, CollectiveOperation::Sum, 12);
    require(endpoint.trySubmitCollective(invalid) == CollectiveSubmitResult::Invalid, "Invalid outcome missing");
    requirePendingUnchanged(invalid, 12, &source, &result, CollectiveOperation::Sum);

    endpoint.setRetry(false);
    CollectivePending accepted = makePending(participant, &source, &result, CollectiveOperation::Sum, 13);
    require(endpoint.trySubmitCollective(accepted) == CollectiveSubmitResult::Accepted,
        "Accepted outcome missing");
    require(accepted.state == CollectivePendingState::Consumed && !accepted.completion.valid(),
        "Accepted did not consume pending token exactly once");
    completion.observed_result = &result;
    completion.expected_result = 9.0;
    require(endpoint.finish(9.0), "accepted completion did not finish");
    require(completion.count == 1 && completion.last_request == 13 &&
                completion.last_status == CollectiveCompletionStatus::Success && completion.result_was_visible,
        "completion was not exactly once after result publication");
    require(!endpoint.finish(10.0) && completion.count == 1, "duplicate completion was delivered");
}

} // namespace

ContractTest::ContractTest(SST::ComponentId_t id, SST::Params& params) : SST::Component(id)
{
    (void)params;
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();

    SST::Output output("", 1, 0, SST::Output::STDOUT);
    try {
        testDescriptor();
        testEndpoint();
    }
    catch ( const std::exception& error ) {
        output.fatal(CALL_INFO, -1, "collective contract FAIL: %s\n", error.what());
    }

    output.output("collective contract PASS\n");
    primaryComponentOKToEndSim();
}

} // namespace SST::Collective
