// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "sst_config.h"

#include "networkServiceTest.h"

#include "../../interfaces/ExtendedRequest.h"
#include "../../router.h"
#include "../request_ownership/cloneTrackingEvent.h"
#include "pr2IntegrationFixture.h"

#include <sst/core/interfaces/stringEvent.h>
#include <sst/core/output.h>

#include <memory>
#include <stdexcept>
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
testServiceDataCopyOwnership()
{
    // Copy, assignment and move of an ExtendedRequest must deep-copy both
    // the native payload and the service sidecar, and destroy each once.
    int clones = 0;
    int destructions = 0;
    {
        ExtendedRequest source(1, 0, 64, true, true, new CloneTrackingEvent(clones, destructions));
        source.giveServiceData(new PR2IntegrationServiceData(PR2IntegrationAction::Pass, 7));
        ExtendedRequest copy(source);
        ExtendedRequest assigned;
        assigned = source;
        ExtendedRequest moved(std::move(copy));
        require(clones == 2 && moved.inspectPayload() != source.inspectPayload() &&
                assigned.inspectPayload() != source.inspectPayload() &&
                moved.inspectServiceData() != nullptr && moved.inspectServiceData() != source.inspectServiceData() &&
                assigned.inspectServiceData() != nullptr && assigned.inspectServiceData() != source.inspectServiceData(),
            "ExtendedRequest copy/move aliased its payload or service sidecar");
    }
    require(destructions == 3, "ExtendedRequest copies did not destroy payloads exactly once");

    // clone() must deep-copy the sidecar as well as the payload.
    ExtendedRequest original(7, 3, 64, true, true, new SST::Interfaces::StringEvent("payload"));
    original.giveServiceData(new PR2IntegrationServiceData(PR2IntegrationAction::Pass, 17));
    std::unique_ptr<ExtendedRequest> cloned(original.clone());
    const auto* cloned_service = cloned->inspectServiceDataAs<PR2IntegrationServiceData>();
    require(cloned_service != nullptr && cloned_service != original.inspectServiceData() &&
            cloned_service->action() == PR2IntegrationAction::Pass && cloned_service->sequence() == 17,
        "ExtendedRequest clone sliced or aliased its service data");

    clones = destructions = 0;
    {
        internal_router_event original(new RtrEvent(new SST::Interfaces::SimpleNetwork::Request(
            1, 0, 64, true, true, new CloneTrackingEvent(clones, destructions)), 0, 0));
        internal_router_event copy(original);
        require(clones == 1 && copy.getEncapsulatedEvent() != original.getEncapsulatedEvent() &&
                copy.inspectRequest()->inspectPayload() != original.inspectRequest()->inspectPayload(),
            "router envelope copy construction aliased ownership");
    }
    require(destructions == 2, "router envelope copies leaked payloads");
}

} // namespace

NetworkServiceTest::NetworkServiceTest(SST::ComponentId_t id, SST::Params& params) : SST::Component(id)
{
    (void)params;
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();

    SST::Output output("", 1, 0, SST::Output::STDOUT);
    try {
        testServiceDataCopyOwnership();
    }
    catch ( const std::exception& error ) {
        output.fatal(CALL_INFO, -1, "Merlin network-service acceptance contract FAIL: %s\n", error.what());
    }

    output.output("Merlin network-service acceptance contract PASS\n");
    primaryComponentOKToEndSim();
}

} // namespace SST::Merlin
