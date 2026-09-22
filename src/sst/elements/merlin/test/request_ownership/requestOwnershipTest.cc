// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "sst_config.h"

#include "../../interfaces/ExtendedRequest.h"
#include "../../router.h"
#include "cloneTrackingEvent.h"

#include <sst/core/component.h>
#include <sst/core/interfaces/stringEvent.h>
#include <sst/core/output.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace SST::Merlin {
namespace {

using SST::Merlin::Test::CloneTrackingEvent;

void
require(bool condition, const char* message)
{
    if ( !condition ) throw std::runtime_error(message);
}

void
testExtendedRequestClone()
{
    ExtendedRequest original(7, 3, 64, true, true, new SST::Interfaces::StringEvent("payload"));
    original.setMetadata("Reorder", ReorderMetadata(11));

    std::unique_ptr<ExtendedRequest> copy(original.clone());
    ReorderMetadata copied_metadata;
    require(copy->inspectPayload() != nullptr && copy->inspectPayload() != original.inspectPayload(),
        "ExtendedRequest clone aliased its native payload");
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

} // namespace

/** Exercises the released ownership contracts of Merlin's request and event envelopes. */
class RequestOwnershipTest final : public SST::Component
{
public:
    SST_ELI_REGISTER_COMPONENT(RequestOwnershipTest, "merlin", "request_ownership_test",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Merlin request and router-event ownership contract test",
        COMPONENT_CATEGORY_UNCATEGORIZED)

    SST_ELI_DOCUMENT_PARAMS()

    RequestOwnershipTest(SST::ComponentId_t id, SST::Params&) : SST::Component(id)
    {
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();

        SST::Output output("", 1, 0, SST::Output::STDOUT);
        try {
            testExtendedRequestClone();
            testExtendedRequestCopyAndMoveOwnership();
            testExtendedRequestLegacyPointerWrapper();
            testExtendedRequestCopyExceptionSafety();
            testLegacyRouterEventAndEnvelopeOwnership();
        }
        catch ( const std::exception& error ) {
            output.fatal(CALL_INFO, -1, "Merlin request ownership contract FAIL: %s\n", error.what());
        }

        output.output("Merlin request ownership contract PASS\n");
        primaryComponentOKToEndSim();
    }
};

} // namespace SST::Merlin
