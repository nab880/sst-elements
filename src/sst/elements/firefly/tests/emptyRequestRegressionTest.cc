// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

#include <sst_config.h>

#include "sst/elements/firefly/tests/emptyRequestRegressionTest.h"

namespace SST::Firefly {

using SimpleNetwork = SST::Interfaces::SimpleNetwork;

EmptyRequestRegressionTest::EmptyRequestRegressionTest(ComponentId_t id, Params& params) :
    Component(id),
    destination_(params.find<SimpleNetwork::nid_t>("destination", 1)),
    vn_(params.find<int>("vn", 0)),
    request_bits_(params.find<size_t>("request_bits", 64))
{
    network_ = loadUserSubComponent<SimpleNetwork>("networkIF", ComponentInfo::SHARE_NONE, 1);
    nic_link_ = configureLink("nic");
    if ( network_ == nullptr || nic_link_ == nullptr || destination_ < 0 ||
         vn_ != 0 || request_bits_ == 0 ) {
        fail("invalid fixture configuration");
    }

    inject_link_ = configureSelfLink("empty_request_inject", "1ns",
        new SST::Event::Handler<EmptyRequestRegressionTest,
            &EmptyRequestRegressionTest::inject>(this));
    deadline_link_ = configureSelfLink("empty_request_deadline", "1ns",
        new SST::Event::Handler<EmptyRequestRegressionTest,
            &EmptyRequestRegressionTest::deadline>(this));
    if ( inject_link_ == nullptr || deadline_link_ == nullptr ) {
        fail("could not configure self links");
    }

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

void EmptyRequestRegressionTest::init(unsigned int phase)
{
    network_->init(phase);
    while ( auto* event = nic_link_->recvUntimedData() ) {
        delete event;
        if ( nic_initialized_ ) fail("received duplicate NIC initialization");
        nic_initialized_ = true;
    }
}

void EmptyRequestRegressionTest::setup()
{
    network_->setup();
    if ( !nic_initialized_ ) fail("did not receive NIC initialization");
    inject_link_->send(1, nullptr);
}

void EmptyRequestRegressionTest::finish()
{
    network_->finish();
}

void EmptyRequestRegressionTest::inject(SST::Event* event)
{
    delete event;
    if ( !network_->isNetworkInitialized() || sent_ ) {
        fail("network was not initialized exactly once");
    }

    auto* request = new SimpleNetwork::Request(
        destination_, network_->getEndpointID(), request_bits_, true, true);
    request->vn = vn_;
    if ( request->inspectPayload() != nullptr || request->hasService() ) {
        delete request;
        fail("fresh Request was unexpectedly tagged or populated");
    }
    if ( !network_->send(request, vn_) ) {
        delete request;
        fail("injector had no initial network credit");
    }
    sent_ = true;
    deadline_link_->send(200, nullptr);
}

void EmptyRequestRegressionTest::deadline(SST::Event* event)
{
    delete event;
    if ( !sent_ ) fail("empty Request was not injected");
    getSimulationOutput().output(
        "Firefly ordinary empty Request: destination=%" PRId64 " vn=%d PASS\n",
        static_cast<int64_t>(destination_), vn_);
    primaryComponentOKToEndSim();
}

[[noreturn]] void EmptyRequestRegressionTest::fail(const char* reason) const
{
    getSimulationOutput().fatal(CALL_INFO, 1,
        "Firefly ordinary empty Request regression failed: %s\n", reason);
}

} // namespace SST::Firefly
