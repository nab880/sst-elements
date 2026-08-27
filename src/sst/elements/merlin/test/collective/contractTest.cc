// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "sst_config.h"

#include "contractTest.h"

#include <sst/elements/merlin/services/collective/collectiveArithmetic.h>
#include <sst/elements/merlin/services/collective/collectiveRoute.h>

#include <sst/core/output.h>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
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

uint64_t readU64(const std::vector<uint8_t>& bytes, size_t offset)
{
    uint64_t value = 0;
    for ( size_t i = 0; i < 8; ++i ) value = (value << 8) | bytes[offset + i];
    return value;
}

void writeU64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value)
{
    for ( int i = 7; i >= 0; --i ) {
        bytes[offset + static_cast<size_t>(i)] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

CollectiveDescriptorFieldsV1 makeFields()
{
    CollectiveDescriptorFieldsV1 fields;
    fields.route                    = { 0x0102030405060708ull, 0 };
    fields.invocation_id            = 0x1112131415161718ull;
    fields.chunk_index              = 0;
    fields.total_chunks             = 1;
    fields.element_offset           = 0;
    fields.element_count            = 1;
    fields.total_elements           = 1;
    fields.operation                = CollectiveOperation::Sum;
    fields.datatype                 = CollectiveDatatype::F64;
    fields.direction                = CollectiveDirection::Contribution;
    fields.logical_payload_bytes    = 8;
    fields.modeled_wire_bytes       = 32;
    fields.data_present             = 1;
    return fields;
}

CollectiveServiceData makeServiceData(double value = 7.0)
{
    std::vector<uint8_t> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return CollectiveServiceData(makeFields(), std::move(bytes));
}

CollectiveRouteRuntimeV1 makeRuntime()
{
    CollectiveRouteRuntimeV1 runtime;
    runtime.route                       = makeFields().route;
    runtime.route_kind                  = CollectiveRouteKind::FabricTree;
    runtime.data_mode                   = CollectiveDataMode::Functional;
    runtime.operation_mask              = operationMask(CollectiveOperation::Sum);
    runtime.datatype_mask               = datatypeMask(CollectiveDatatype::F64);
    runtime.accepted_invocation_quota   = 1;
    runtime.submission_window            = 1;
    runtime.maximum_logical_chunk_bytes = 8;
    runtime.maximum_chunk_elements[5]   = 1;
    runtime.fabric.emplace();
    runtime.fabric->endpoint_reduce_vn   = 1;
    runtime.fabric->endpoint_result_vn   = 2;
    runtime.fabric->fabric_reduce_vn     = 3;
    runtime.fabric->fabric_result_vn     = 4;
    runtime.fabric->service_header_bytes = 16;
    runtime.fabric->fabric_framing_bytes = 8;
    runtime.fabric->accepted_flit_bits    = 64;
    runtime.fabric->maximum_request_bits  = 256;
    return runtime;
}

AcceptedParticipantHandle makeParticipant(uint32_t slot = 0)
{
    AcceptedParticipantHandle participant;
    participant.route                       = makeFields().route;
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

    const std::vector<uint8_t> canonical = data.canonicalBytes();
    require(canonical.size() == CollectiveServiceData::FIXED_PREFIX_BYTES + 8, "wrong canonical size");
    require(canonical[0] == 0 && canonical[1] == 1 && canonical[2] == 0 && canonical[3] == 1,
        "version fields moved");
    require(readU64(canonical, 4) == data.fields.route.job_namespace, "namespace offset changed");
    require(readU64(canonical, 12) == 0, "route offset changed");
    require(readU64(canonical, 20) == data.fields.invocation_id, "invocation offset changed");
    require(canonical[60] == 1 && canonical[61] == 6, "operation/datatype offsets changed");
    require(canonical[62] == 0 && canonical[63] == 1 && canonical[64] == 1,
        "policy/direction offsets changed");
    require(readU64(canonical, 65) == 8 && readU64(canonical, 73) == 32 && canonical[81] == 1 &&
                readU64(canonical, 82) == 8,
        "payload metadata offsets changed");

    CollectiveServiceData decoded;
    require(CollectiveServiceData::decodeCanonical(canonical.data(), canonical.size(), decoded) ==
                DescriptorValidation::Valid,
        "canonical decode failed");
    require(decoded.fields.route == data.fields.route && decoded.fields.invocation_id == data.fields.invocation_id &&
                decoded.owned_bytes == data.owned_bytes,
        "canonical decode changed data");

    for ( size_t length = 0; length < canonical.size(); ++length ) {
        CollectiveServiceData truncated;
        require(CollectiveServiceData::decodeCanonical(canonical.data(), length, truncated) !=
                    DescriptorValidation::Valid,
            "truncated descriptor accepted");
    }

    auto malformed = canonical;
    malformed[1]   = 2;
    require(CollectiveServiceData::decodeCanonical(malformed.data(), malformed.size(), decoded) ==
                DescriptorValidation::InvalidServiceSchema,
        "bad service schema accepted");
    malformed     = canonical;
    malformed[60] = 0;
    require(CollectiveServiceData::decodeCanonical(malformed.data(), malformed.size(), decoded) ==
                DescriptorValidation::InvalidOperation,
        "bad operation accepted");
    malformed     = canonical;
    malformed[61] = 0;
    require(CollectiveServiceData::decodeCanonical(malformed.data(), malformed.size(), decoded) ==
                DescriptorValidation::InvalidDatatype,
        "bad datatype accepted");
    malformed     = canonical;
    malformed[64] = 0;
    require(CollectiveServiceData::decodeCanonical(malformed.data(), malformed.size(), decoded) ==
                DescriptorValidation::InvalidDirection,
        "bad direction accepted");
    malformed     = canonical;
    malformed[81] = 2;
    require(CollectiveServiceData::decodeCanonical(malformed.data(), malformed.size(), decoded) ==
                DescriptorValidation::InvalidDataPresent,
        "bad data-present code accepted");
    malformed = canonical;
    writeU64(malformed, 82, 7);
    require(CollectiveServiceData::decodeCanonical(malformed.data(), malformed.size(), decoded) ==
                DescriptorValidation::EncodedLengthMismatch,
        "bad owned length accepted");
    malformed = canonical;
    std::fill(malformed.begin() + 4, malformed.begin() + 12, 0);
    require(CollectiveServiceData::decodeCanonical(malformed.data(), malformed.size(), decoded) ==
                DescriptorValidation::InvalidNamespace,
        "zero namespace accepted");
    malformed = canonical;
    std::fill(malformed.begin() + 32, malformed.begin() + 36, 0);
    require(CollectiveServiceData::decodeCanonical(malformed.data(), malformed.size(), decoded) ==
                DescriptorValidation::InvalidChunkCount,
        "zero total-chunk count accepted");

    SimpleNetwork::Request request(7, 9, 256, true, true);
    request.vn = 1;
    request.giveServiceData(new CollectiveServiceData(data));
    std::unique_ptr<SimpleNetwork::Request> cloned(request.clone());
    require(cloned->getServiceID() == COLLECTIVE_SERVICE_ID &&
                cloned->inspectServiceData() != request.inspectServiceData() &&
                cloned->inspectServiceDataAs<CollectiveServiceData>() != nullptr,
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

    const CollectiveRouteRuntimeV1 runtime = makeRuntime();
    require(validateCollectivePacket(data, runtime, CollectiveIngressRole::LocalEndpointContribution, 256) ==
                CollectivePacketValidation::Valid,
        "valid route-context packet rejected");
    data.fields.direction = CollectiveDirection::Result;
    require(validateCollectivePacket(data, runtime, CollectiveIngressRole::ChildContribution, 256) ==
                CollectivePacketValidation::DirectionMismatch,
        "wrong ingress direction accepted");
    data.fields.direction         = CollectiveDirection::Contribution;
    data.fields.modeled_wire_bytes = 31;
    require(validateCollectivePacket(data, runtime, CollectiveIngressRole::ChildContribution, 248) ==
                CollectivePacketValidation::ModeledSizeMismatch,
        "wrong modeled size accepted");
}

void testArithmetic()
{
    std::array<double, 2> a { 1.0, -3.0 };
    std::array<double, 2> b { 2.0, 1.0 };
    std::array<double, 2> c { 4.0, -5.0 };
    std::array<double, 2> result { 99.0, 99.0 };
    const std::array<BufferView, 3> inputs {
        BufferView { reinterpret_cast<const uint8_t*>(a.data()), sizeof(a) },
        BufferView { reinterpret_cast<const uint8_t*>(b.data()), sizeof(b) },
        BufferView { reinterpret_cast<const uint8_t*>(c.data()), sizeof(c) }
    };
    require(reduceOrdered(CollectiveOperation::Sum, CollectiveDatatype::F64, 2, inputs.data(), inputs.size(),
                { reinterpret_cast<uint8_t*>(result.data()), sizeof(result) }) == ArithmeticStatus::Success &&
                result[0] == 7.0 && result[1] == -7.0,
        "ordered F64 sum is wrong");

    std::array<uint8_t, 17> unaligned_a {};
    std::array<uint8_t, 17> unaligned_b {};
    std::array<uint8_t, 17> unaligned_result {};
    const double ua = 2.0;
    const double ub = 5.0;
    std::memcpy(unaligned_a.data() + 1, &ua, 8);
    std::memcpy(unaligned_b.data() + 1, &ub, 8);
    const std::array<BufferView, 2> unaligned_inputs {
        BufferView { unaligned_a.data() + 1, 8 }, BufferView { unaligned_b.data() + 1, 8 }
    };
    require(reduceOrdered(CollectiveOperation::Sum, CollectiveDatatype::F64, 1, unaligned_inputs.data(), 2,
                { unaligned_result.data() + 1, 8 }) == ArithmeticStatus::Success,
        "unaligned F64 sum failed");
    double unaligned_value;
    std::memcpy(&unaligned_value, unaligned_result.data() + 1, 8);
    require(unaligned_value == 7.0, "unaligned F64 result is wrong");

    std::array<double, 1> in_place { 3.0 };
    std::array<double, 1> addend { 4.0 };
    const std::array<BufferView, 2> alias_inputs {
        BufferView { reinterpret_cast<const uint8_t*>(in_place.data()), 8 },
        BufferView { reinterpret_cast<const uint8_t*>(addend.data()), 8 }
    };
    require(reduceOrdered(CollectiveOperation::Sum, CollectiveDatatype::F64, 1, alias_inputs.data(), 2,
                { reinterpret_cast<uint8_t*>(in_place.data()), 8 }) == ArithmeticStatus::Success &&
                in_place[0] == 7.0,
        "exact in-place alias failed");

    const auto unchanged = result;
    require(reduceOrdered(CollectiveOperation::Min, CollectiveDatatype::F64, 2, inputs.data(), inputs.size(),
                { reinterpret_cast<uint8_t*>(result.data()), sizeof(result) }) == ArithmeticStatus::Unsupported &&
                result == unchanged,
        "unsupported arithmetic changed output");
    require(reduceOrdered(CollectiveOperation::Sum, CollectiveDatatype::F64, 2, inputs.data(), inputs.size(),
                { reinterpret_cast<uint8_t*>(result.data()), 8 }) == ArithmeticStatus::Invalid && result == unchanged,
        "invalid arithmetic changed output");

    std::array<uint8_t, 16> overlap {};
    const BufferView overlap_input { overlap.data(), 8 };
    require(reduceOrdered(CollectiveOperation::Sum, CollectiveDatatype::F64, 1, &overlap_input, 1,
                { overlap.data() + 1, 8 }) == ArithmeticStatus::Invalid,
        "partial overlap accepted");
    require(reduceOrdered(CollectiveOperation::Sum, CollectiveDatatype::F64, 0,
                reinterpret_cast<const BufferView*>(uintptr_t { 1 }), UINT32_MAX,
                { reinterpret_cast<uint8_t*>(uintptr_t { 1 }), UINT64_MAX }) == ArithmeticStatus::Success,
        "zero-element arithmetic dereferenced poison state");
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

void testRoutes()
{
    CollectiveRouteRuntimeV1 runtime = makeRuntime();
    require(runtime.valid(), "valid runtime route rejected");

    EndpointRouteProjectionV1 endpoint;
    endpoint.route                   = runtime.route;
    endpoint.owner_component_id      = 44;
    endpoint.physical_endpoint_id    = 9;
    endpoint.local_participant_count = 1;
    endpoint.local_participants.push_back({ 0, 100, 0, { 44, 0, 1 } });
    endpoint.fabric.emplace();
    endpoint.fabric->root_representative = { 7, 7 };
    endpoint.fabric->injection_dest_nid  = 7;
    require(endpoint.valid(runtime), "valid endpoint-local projection rejected");
    endpoint.local_participants[0].local_slot = 1;
    require(!endpoint.valid(runtime), "nondense local slot accepted");
    endpoint.local_participants[0].local_slot = 0;

    RouterRouteProjectionV1 root;
    root.route                  = runtime.route;
    root.owner_component_id     = 70;
    root.root                   = true;
    root.subtree_representative = { 7, 7 };
    root.root_representative    = { 7, 7 };
    root.child_branches.push_back({ 1, { 9, 9 } });
    root.local_endpoint_branches.push_back({ 2, { 7, 7 } });
    require(root.valid(runtime), "valid root-local projection rejected");
    root.local_endpoint_branches[0].port = 1;
    require(!root.valid(runtime), "duplicate local router port accepted");
    root.local_endpoint_branches[0].port = 2;

    RouterRouteProjectionV1 leaf;
    leaf.route                  = runtime.route;
    leaf.owner_component_id     = 71;
    leaf.root                   = false;
    leaf.parent_port            = 3;
    leaf.subtree_representative = { 9, 9 };
    leaf.root_representative    = { 7, 7 };
    leaf.local_endpoint_branches.push_back({ 0, { 9, 9 } });
    require(leaf.valid(runtime), "valid leaf-local projection rejected");
    leaf.parent_port.reset();
    require(!leaf.valid(runtime), "nonroot projection without parent accepted");

    CollectiveRouteRuntimeV1 local_runtime = runtime;
    local_runtime.route_kind               = CollectiveRouteKind::EndpointLocal;
    local_runtime.fabric.reset();
    EndpointRouteProjectionV1 local_endpoint = endpoint;
    local_endpoint.fabric.reset();
    require(local_runtime.valid() && local_endpoint.valid(local_runtime),
        "EndpointLocal runtime requires fabric/global state");
    require(root.validate(local_runtime) == CollectiveRouteValidation::RouteKindMismatch,
        "EndpointLocal route accepted a router projection");
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
        testArithmetic();
        testEndpoint();
        testRoutes();
    }
    catch ( const std::exception& error ) {
        output.fatal(CALL_INFO, -1, "collective contract FAIL: %s\n", error.what());
    }

    output.output("collective contract PASS\n");
    primaryComponentOKToEndSim();
}

} // namespace SST::Collective
