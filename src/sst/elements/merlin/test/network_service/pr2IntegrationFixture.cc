// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include <sst_config.h>

#include "sst/elements/merlin/test/network_service/pr2IntegrationFixture.h"

#include <sst/core/output.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace SST::Merlin {

void
PR2IntegrationServiceData::serialize_order(SST::Core::Serialization::serializer& ser)
{
    SST_SER(action_);
    SST_SER(sequence_);
}

PR2MissingProcessorEndpoint::PR2MissingProcessorEndpoint(ComponentId_t id, Params& params) :
    Component(id),
    expect_processor_(params.find<bool>("expect_processor", false))
{
    network_ = loadUserSubComponent<SST::Interfaces::SimpleNetwork>(
        "networkIF", ComponentInfo::SHARE_NONE, 1);
    if ( network_ == nullptr ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "Missing-processor probe requires a networkIF\n");
    }
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

void
PR2MissingProcessorEndpoint::init(unsigned int phase)
{
    network_->init(phase);
}

void
PR2MissingProcessorEndpoint::setup()
{
    network_->setup();
    if ( !network_->isNetworkInitialized() ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "Network was not initialized before service probing\n");
    }

    if ( expect_processor_ ) {
        SST::Interfaces::SimpleNetwork::NetworkServiceCapability capability;
        if ( !network_->queryServiceCapability(PR2_INTEGRATION_SERVICE_ID, capability) ||
             !capability.isValidFor(PR2_INTEGRATION_SERVICE_ID) ) {
            getSimulationOutput().fatal(CALL_INFO, 1,
                "Production PASS processor was not advertised as a network service\n");
        }

        network_->setNotifyOnReceive(new SST::Interfaces::SimpleNetwork::Handler<
            PR2MissingProcessorEndpoint, &PR2MissingProcessorEndpoint::handleReceive>(this));
        auto request = std::make_unique<SST::Interfaces::SimpleNetwork::Request>(0, 0, 64, true, true);
        request->vn = 0;
        request->giveServiceData(new PR2IntegrationServiceData(PR2IntegrationAction::Pass, 100));
        if ( !network_->send(request.get(), 0) ) {
            getSimulationOutput().fatal(CALL_INFO, 1,
                "Production PASS processor test could not inject its tagged packet\n");
        }
        request.release();
        return;
    }

    SST::Interfaces::SimpleNetwork::NetworkServiceCapability sentinel;
    sentinel.service_id = 17;
    sentinel.min_schema_version = 3;
    sentinel.max_schema_version = 9;
    sentinel.features = 0x55;
    sentinel.max_atomic_request_bits_by_vn = { 123 };
    if ( network_->queryServiceCapability(PR2_INTEGRATION_SERVICE_ID, sentinel) ||
         sentinel.service_id != 17 || sentinel.min_schema_version != 3 ||
         sentinel.max_schema_version != 9 || sentinel.features != 0x55 ||
         sentinel.max_atomic_request_bits_by_vn != std::vector<uint64_t>{ 123 } ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "Missing-processor capability query was not false and transactional\n");
    }

    auto request = std::make_unique<SST::Interfaces::SimpleNetwork::Request>(0, 0, 64, true, true);
    request->vn = 7;
    request->giveServiceData(new PR2IntegrationServiceData(PR2IntegrationAction::Pass, 99));
    const auto* sidecar = request->inspectServiceData();
    if ( network_->send(request.get(), 0) || request->dest != 0 || request->src != 0 ||
         request->vn != 7 || request->size_in_bits != 64 || request->inspectServiceData() != sidecar ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "Missing-processor send did not fail without mutating caller ownership\n");
    }

    getSimulationOutput().output("Merlin PR2 missing processor: PASS\n");
    primaryComponentOKToEndSim();
}

bool
PR2MissingProcessorEndpoint::handleReceive(int vn)
{
    if ( !expect_processor_ || vn != 0 || !network_->requestToReceive(vn) ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "Production PASS processor test received an invalid notification\n");
    }

    std::unique_ptr<SST::Interfaces::SimpleNetwork::Request> request(network_->recv(vn));
    const auto* data = request == nullptr ? nullptr :
        request->inspectServiceDataAs<PR2IntegrationServiceData>();
    if ( data == nullptr || data->action() != PR2IntegrationAction::Pass ||
         data->sequence() != 100 || request->src != 0 || request->dest != 0 ||
         request->vn != 0 || request->size_in_bits != 64 ||
         network_->requestToReceive(vn) ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "Production PASS processor changed or duplicated its tagged packet\n");
    }

    getSimulationOutput().output("Merlin PR2 production PASS processor: PASS\n");
    primaryComponentOKToEndSim();
    return true;
}

void
PR2MissingProcessorEndpoint::complete(unsigned int phase)
{
    network_->complete(phase);
}

void
PR2MissingProcessorEndpoint::finish()
{
    network_->finish();
}

PR2VNRemapEndpoint::PR2VNRemapEndpoint(ComponentId_t id, Params& params) : Component(id)
{
    params.find_array<int>("expected_map", expected_map_);
    if ( expected_map_.size() != 3 || std::any_of(expected_map_.begin(), expected_map_.end(),
            [](int vn) { return vn < 0 || vn >= 3; }) ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe requires three valid expected VNs\n");
    }
    network_ = loadUserSubComponent<SimpleNetwork>("networkIF", ComponentInfo::SHARE_NONE, 3);
    if ( network_ == nullptr ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe requires a networkIF\n");
    }
    deadline_ = configureSelfLink("vn_remap_deadline", "100ns",
        new SST::Event::Handler<PR2VNRemapEndpoint, &PR2VNRemapEndpoint::handleDeadline>(this));
    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

void PR2VNRemapEndpoint::init(unsigned int phase) { network_->init(phase); }
void PR2VNRemapEndpoint::complete(unsigned int phase) { network_->complete(phase); }
void PR2VNRemapEndpoint::finish() { network_->finish(); }

void
PR2VNRemapEndpoint::setup()
{
    network_->setup();
    if ( !network_->isNetworkInitialized() || network_->getEndpointID() != 0 ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe network was not initialized as endpoint 0\n");
    }
    bool has_identity = false;
    for ( int vn = 0; vn < 3; ++vn ) has_identity |= expected_map_[vn] == vn;
    SimpleNetwork::NetworkServiceCapability capability;
    capability.service_id = 17;
    capability.max_atomic_request_bits_by_vn = { 123 };
    const bool supported = network_->queryServiceCapability(PR2_INTEGRATION_SERVICE_ID, capability);
    if ( supported != has_identity ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe advertised the wrong service support\n");
    }
    if ( supported ) {
        if ( !capability.isValidFor(PR2_INTEGRATION_SERVICE_ID) ||
             capability.max_atomic_request_bits_by_vn.size() != 3 ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe received an invalid capability\n");
        }
        for ( int vn = 0; vn < 3; ++vn ) {
            const uint64_t expected = expected_map_[vn] == vn ? 64 : 0;
            if ( capability.max_atomic_request_bits_by_vn[vn] != expected ) {
                getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe advertised incorrect capacity on VN %d\n", vn);
            }
        }
    }
    else if ( capability.service_id != 17 ||
              capability.max_atomic_request_bits_by_vn != std::vector<uint64_t>{ 123 } ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap unsupported capability query changed its output\n");
    }

    // Probe before any accepted sends. Exactly one flit must remain available
    // even when several logical VNs alias the same output queue.
    for ( int vn = 0; vn < 3; ++vn ) {
        if ( !network_->spaceToSend(vn, 64) || network_->spaceToSend(vn, 65) ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe requires one-flit endpoint queues\n");
        }
        if ( expected_map_[vn] == vn ) continue;
        auto request = std::make_unique<SimpleNetwork::Request>(0, 37, 64, true, false, new SST::Event());
        request->vn = 71;
        request->allow_adaptive = false;
        request->giveServiceData(new PR2IntegrationServiceData(PR2IntegrationAction::Pass, 99));
        const auto* payload = request->inspectPayload();
        const auto* sidecar = request->inspectServiceData();
        for ( int attempt = 0; attempt < 2; ++attempt ) {
            if ( network_->send(request.get(), vn) || request->dest != 0 || request->src != 37 ||
                 request->vn != 71 || request->size_in_bits != 64 || !request->head || request->tail ||
                 request->allow_adaptive || request->getServiceID() != PR2_INTEGRATION_SERVICE_ID ||
                 request->inspectPayload() != payload || request->inspectServiceData() != sidecar ||
                 !network_->spaceToSend(vn, 64) || network_->spaceToSend(vn, 65) ) {
                getSimulationOutput().fatal(CALL_INFO, 1,
                    "VN-remap tagged rejection changed request ownership, fields, or credits on VN %d\n", vn);
            }
        }
    }

    for ( int vn = 0; vn < 3; ++vn ) {
        auto ordinary = std::make_unique<SimpleNetwork::Request>(0, 0, 64, true, true);
        ordinary->vn = vn;
        pending_.push_back(std::move(ordinary));
        if ( expected_map_[vn] == vn ) {
            auto tagged = std::make_unique<SimpleNetwork::Request>(0, 0, 64, true, true);
            tagged->vn = vn;
            tagged->giveServiceData(new PR2IntegrationServiceData(PR2IntegrationAction::Pass, 100 + vn));
            pending_.push_back(std::move(tagged));
        }
    }
    network_->setNotifyOnReceive(new SimpleNetwork::Handler<PR2VNRemapEndpoint,
        &PR2VNRemapEndpoint::handleReceive>(this));
    if ( handleSend(0) ) {
        network_->setNotifyOnSend(new SimpleNetwork::Handler<PR2VNRemapEndpoint,
            &PR2VNRemapEndpoint::handleSend>(this));
    }
    deadline_->send(1, nullptr);
}

bool
PR2VNRemapEndpoint::handleSend(int)
{
    while ( next_to_send_ < pending_.size() ) {
        auto& request = pending_[next_to_send_];
        if ( !network_->send(request.get(), request->vn) ) return true;
        request.release();
        ++next_to_send_;
    }
    return false;
}

bool
PR2VNRemapEndpoint::handleReceive(int vn)
{
    if ( vn < 0 || vn >= 3 ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe received an invalid VN\n");
    }
    while ( network_->requestToReceive(vn) ) {
        std::unique_ptr<SimpleNetwork::Request> request(network_->recv(vn));
        if ( request == nullptr || request->vn != vn || request->src != 0 || request->dest != 0 ||
             request->size_in_bits != 64 || !request->head || !request->tail ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe received altered packet metadata\n");
        }
        if ( request->hasService() ) {
            const auto* data = request->inspectServiceDataAs<PR2IntegrationServiceData>();
            if ( expected_map_[vn] != vn || data == nullptr || data->action() != PR2IntegrationAction::Pass ||
                 data->sequence() != static_cast<uint32_t>(100 + vn) || ++tagged_received_[vn] != 1 ) {
                getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe received unexpected or duplicate service data\n");
            }
        }
        else if ( ++ordinary_received_[vn] != 1 ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe received duplicate ordinary traffic\n");
        }
    }
    return true;
}

void
PR2VNRemapEndpoint::handleDeadline(SST::Event* event)
{
    delete event;
    if ( next_to_send_ != pending_.size() ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe failed to send all traffic\n");
    }
    for ( int vn = 0; vn < 3; ++vn ) {
        if ( ordinary_received_[vn] != 1 || tagged_received_[vn] != unsigned(expected_map_[vn] == vn) ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "VN-remap probe lost traffic on VN %d\n", vn);
        }
    }
    getSimulationOutput().output("Merlin VN remap %s: PASS\n", getName().c_str());
    primaryComponentOKToEndSim();
}

} // namespace SST::Merlin
