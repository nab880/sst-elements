// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include <sst_config.h>

#include "sst/elements/merlin/test/network_service/pr2IntegrationFixture.h"

#include "sst/elements/merlin/hr_router/hr_router.h"
#include "sst/elements/merlin/router.h"

#include <sst/core/output.h>

#include <algorithm>
#include <inttypes.h>
#include <utility>
#include <vector>

namespace SST::Merlin {

void
PR2IntegrationServiceData::serialize_order(SST::Core::Serialization::serializer& ser)
{
    SST_SER(action_);
    SST_SER(sequence_);
}

PR2IntegrationProcessor::PR2IntegrationProcessor(
    ComponentId_t id, Params&, NetworkServiceHost* host) :
    NetworkServiceProcessor(id, host)
{
    if ( host == nullptr ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration processor requires a network-service host\n");
    }
    trigger_ = configureLink("trigger",
        new SST::Event::Handler<PR2IntegrationProcessor, &PR2IntegrationProcessor::handleTrigger>(this));
    if ( trigger_ == nullptr ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration processor requires its trigger port\n");
    }
}

NetworkServiceDecision
PR2IntegrationProcessor::inspect(const NetworkServiceIngress& ingress) const
{
    if ( ingress.input_port < 0 || ingress.input_vn < 0 || ingress.event == nullptr ) {
        return { NetworkServiceDisposition::Reject, 1 };
    }

    const auto* request = ingress.event->inspectRequest();
    const auto* data = request == nullptr ? nullptr : request->inspectServiceDataAs<PR2IntegrationServiceData>();
    if ( data == nullptr ) return { NetworkServiceDisposition::Reject, 2 };

    switch ( data->action() ) {
    case PR2IntegrationAction::Pass:
        // Pass traffic travels on the unowned VN and never reaches inspect().
        return { NetworkServiceDisposition::Reject, 5 };
    case PR2IntegrationAction::AcceptEcho:
        return { NetworkServiceDisposition::Accept };
    case PR2IntegrationAction::BusyUntilEcho:
        return { getCurrentSimTimeNano() < BUSY_RELEASE_NS ? NetworkServiceDisposition::Busy :
                                                            NetworkServiceDisposition::Accept };
    case PR2IntegrationAction::SyntheticEcho:
        return { NetworkServiceDisposition::Reject, 3 };
    }
    return { NetworkServiceDisposition::Reject, 4 };
}

void
PR2IntegrationProcessor::consume(NetworkServiceOwnedIngress ingress) noexcept
{
    const auto* request = ingress.event == nullptr ? nullptr : ingress.event->inspectRequest();
    const auto* data = request == nullptr ? nullptr : request->inspectServiceDataAs<PR2IntegrationServiceData>();
    if ( ingress.input_port < 0 || ingress.input_vn < 0 || data == nullptr ||
         (data->action() != PR2IntegrationAction::AcceptEcho &&
          data->action() != PR2IntegrationAction::BusyUntilEcho) ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "PR2 integration processor consumed an invalid accepted ingress\n");
    }
    const uint32_t sequence = data->sequence();
    emitEcho(std::move(ingress.event), sequence);
}

void
PR2IntegrationProcessor::handleTrigger(SST::Event* event)
{
    delete event;
    auto* router = dynamic_cast<hr_router*>(host());
    if ( router == nullptr || !router->getRequestNotifyOnEvent() ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "PR2 integration external work arrived before the router declocked\n");
    }
    auto echo = std::make_unique<SST::Interfaces::SimpleNetwork::Request>(0, 1, 64, true, true);
    echo->vn = 0;
    echo->giveServiceData(new PR2IntegrationServiceData(PR2IntegrationAction::SyntheticEcho, 4));

    NetworkServiceSyntheticPacket packet;
    packet.request = std::move(echo);
    packet.trusted_src = 1;
    packet.route_vn = 0;
    packet.output_port = 0;
    if ( !host()->tryEnqueueNetworkServiceOutput(PR2_INTEGRATION_SERVICE_ID, packet) ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "PR2 integration processor could not enqueue externally triggered synthetic work\n");
    }
}

void
PR2IntegrationProcessor::emitEcho(
    std::unique_ptr<internal_router_event> event, uint32_t sequence) noexcept
{
    if ( event == nullptr || event->inspectRequest() == nullptr ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration processor committed an empty event\n");
    }

    const auto* original = event->inspectRequest();
    const auto* original_data = original->inspectServiceDataAs<PR2IntegrationServiceData>();
    if ( original_data == nullptr ||
         (original_data->action() != PR2IntegrationAction::AcceptEcho &&
          original_data->action() != PR2IntegrationAction::BusyUntilEcho) ||
         original_data->sequence() != sequence ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration processor committed changed service data\n");
    }

    auto echo = std::make_unique<SST::Interfaces::SimpleNetwork::Request>(
        original->src, original->dest, original->size_in_bits, true, true);
    echo->vn = original->vn;
    echo->allow_adaptive = original->allow_adaptive;
    echo->giveServiceData(new PR2IntegrationServiceData(PR2IntegrationAction::SyntheticEcho, sequence));

    NetworkServiceSyntheticPacket packet;
    packet.trusted_src = echo->src;
    packet.route_vn = event->getVN();
    packet.output_port = static_cast<int>(echo->dest);
    packet.request = std::move(echo);

    if ( !host()->tryEnqueueNetworkServiceOutput(PR2_INTEGRATION_SERVICE_ID, packet) ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration processor could not enqueue its synthetic echo\n");
    }
}

PR2IntegrationEndpoint::PR2IntegrationEndpoint(ComponentId_t id, Params& params) :
    Component(id),
    endpoint_id_(params.find<int>("id", -1))
{
    if ( endpoint_id_ != 0 && endpoint_id_ != 1 ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration endpoint id must be 0 or 1\n");
    }

    // VN 0 is owned by the processor; VN 1 carries ordinary tagged traffic.
    network_ = loadUserSubComponent<SimpleNetwork>("networkIF", ComponentInfo::SHARE_NONE, 2);
    if ( network_ == nullptr ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration endpoint requires a networkIF subcomponent\n");
    }

    deadline_ = configureSelfLink("pr2_deadline", endpoint_id_ == 0 ? "120ns" : "110ns",
        new SST::Event::Handler<PR2IntegrationEndpoint, &PR2IntegrationEndpoint::handleDeadline>(this));
    if ( endpoint_id_ == 0 ) {
        drain_ = configureSelfLink("pr2_drain", "50ns",
            new SST::Event::Handler<PR2IntegrationEndpoint, &PR2IntegrationEndpoint::handleDrain>(this));
        trigger_ = configureLink("service_trigger");
        if ( trigger_ == nullptr ) {
            getSimulationOutput().fatal(CALL_INFO, 1,
                "PR2 integration endpoint 0 requires its service_trigger port\n");
        }
        trigger_timer_ = configureSelfLink("pr2_trigger_timer", "70ns",
            new SST::Event::Handler<PR2IntegrationEndpoint, &PR2IntegrationEndpoint::handleTriggerTimer>(this));
    }

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();
}

void
PR2IntegrationEndpoint::init(unsigned int phase)
{
    network_->init(phase);
}

void
PR2IntegrationEndpoint::setup()
{
    network_->setup();
    if ( network_->getEndpointID() != endpoint_id_ ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "PR2 integration endpoint ID mismatch: configured %d, network reports %" PRI_NID "\n",
            endpoint_id_, network_->getEndpointID());
    }
    validateCapability();

    network_->setNotifyOnReceive(
        new SimpleNetwork::Handler<PR2IntegrationEndpoint, &PR2IntegrationEndpoint::handleReceive>(this));

    if ( endpoint_id_ == 0 ) {
        pending_[0] = makeRequest(PR2IntegrationAction::BusyUntilEcho, 3, 1, 0);
        pending_[1] = makeRequest(PR2IntegrationAction::AcceptEcho, 2, 1, 0);
        pending_count_ = 2;
        trigger_timer_->send(1, nullptr);
        drain_->send(1, nullptr);
    }
    else {
        pending_[0] = makeRequest(PR2IntegrationAction::Pass, 1, 0, 1);
        pending_count_ = 1;
    }
    if ( handleSend(0) ) {
        network_->setNotifyOnSend(
            new SimpleNetwork::Handler<PR2IntegrationEndpoint, &PR2IntegrationEndpoint::handleSend>(this));
    }

    deadline_->send(1, nullptr);
}

void
PR2IntegrationEndpoint::complete(unsigned int phase)
{
    network_->complete(phase);
}

void
PR2IntegrationEndpoint::finish()
{
    network_->finish();
}

bool
PR2IntegrationEndpoint::handleSend(int vn)
{
    if ( vn != 0 && vn != 1 ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration endpoint received an invalid send notification\n");
    }

    while ( next_to_send_ < pending_count_ ) {
        auto& request = pending_[next_to_send_];
        if ( !network_->send(request.get(), request->vn) ) return true;
        request.release();
        ++next_to_send_;
        ++sent_;
    }
    return false;
}

bool
PR2IntegrationEndpoint::handleReceive(int vn)
{
    if ( vn != 0 && vn != 1 ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration endpoint received on an invalid VN\n");
    }

    // Before the drain each VN's one-flit input buffer admits exactly one
    // packet: the Pass packet on VN 1 and the first echo on VN 0.  The
    // second echo must wait for credits.
    if ( endpoint_id_ == 0 && !drain_enabled_ ) {
        if ( ++pre_drain_notifications_ > 2 ) {
            getSimulationOutput().fatal(CALL_INFO, 1,
                "PR2 integration endpoint exceeded one credited receive per VN before its drain\n");
        }
        return true;
    }

    while ( network_->requestToReceive(vn) ) {
        std::unique_ptr<SimpleNetwork::Request> request(network_->recv(vn));
        if ( request == nullptr ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration endpoint observed an empty receive queue head\n");
        }

        const auto* data = request->inspectServiceDataAs<PR2IntegrationServiceData>();
        if ( data == nullptr || request->size_in_bits != 64 || request->vn != vn ||
             !request->head || !request->tail ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration endpoint received malformed service data\n");
        }

        if ( endpoint_id_ == 0 && vn == 1 && data->action() == PR2IntegrationAction::Pass &&
             data->sequence() == 1 && request->src == 1 && request->dest == 0 ) {
            ++pass_received_;
        }
        else if ( endpoint_id_ == 0 && vn == 0 && data->action() == PR2IntegrationAction::SyntheticEcho &&
                  data->sequence() >= 2 && data->sequence() <= 4 &&
                  request->src == 1 && request->dest == 0 ) {
            const uint32_t bit = 1u << data->sequence();
            if ( echo_sequence_mask_ & bit ) {
                getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration endpoint received a duplicate echo\n");
            }
            echo_sequence_mask_ |= bit;
            ++echo_received_;
        }
        else {
            getSimulationOutput().fatal(CALL_INFO, 1,
                "PR2 integration endpoint %d received an unexpected action or address\n", endpoint_id_);
        }
    }
    return true;
}

void
PR2IntegrationEndpoint::handleDrain(SST::Event* event)
{
    delete event;
    if ( pre_drain_notifications_ != 2 ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "PR2 integration endpoint expected exactly one credited receive per VN before its drain\n");
    }
    drain_enabled_ = true;
    handleReceive(0);
    handleReceive(1);
}

void
PR2IntegrationEndpoint::handleTriggerTimer(SST::Event* event)
{
    delete event;
    trigger_->send(new SST::Event());
}

void
PR2IntegrationEndpoint::handleDeadline(SST::Event* event)
{
    delete event;
    const bool passed = endpoint_id_ == 0 ?
        (sent_ == 2 && next_to_send_ == pending_count_ && pass_received_ == 1 && echo_received_ == 3 &&
            echo_sequence_mask_ == ((1u << 2) | (1u << 3) | (1u << 4))) :
        (sent_ == 1 && next_to_send_ == pending_count_ && pass_received_ == 0 && echo_received_ == 0);
    if ( !passed ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "PR2 integration endpoint %d failed: sent=%u pass=%u echo=%u\n",
            endpoint_id_, sent_, pass_received_, echo_received_);
    }
    getSimulationOutput().output(
        "Merlin PR2 integration endpoint %d: sent=%u pass=%u echo=%u PASS\n",
        endpoint_id_, sent_, pass_received_, echo_received_);
    primaryComponentOKToEndSim();
}

std::unique_ptr<PR2IntegrationEndpoint::SimpleNetwork::Request>
PR2IntegrationEndpoint::makeRequest(
    PR2IntegrationAction action, uint32_t sequence, SimpleNetwork::nid_t destination, int vn) const
{
    auto request = std::make_unique<SimpleNetwork::Request>(destination, endpoint_id_, 64, true, true);
    request->vn = vn;
    request->giveServiceData(new PR2IntegrationServiceData(action, sequence));
    return request;
}

void
PR2IntegrationEndpoint::validateCapability() const
{
    SimpleNetwork::NetworkServiceCapability capability;
    const auto required = SimpleNetwork::SERVICE_FEATURE_SIDECAR_PRESERVATION |
                          SimpleNetwork::SERVICE_FEATURE_TRANSACTIONAL_TIMED_SEND |
                          SimpleNetwork::SERVICE_FEATURE_INTERMEDIATE_TERMINATION_SAFE |
                          SimpleNetwork::SERVICE_FEATURE_FRESH_BASE_REQUEST_TAG_FIRST_RECEIVE;
    if ( !network_->queryServiceCapability(PR2_INTEGRATION_SERVICE_ID, capability) ||
         !capability.isValidFor(PR2_INTEGRATION_SERVICE_ID) ||
         (capability.features & required) != required ||
         capability.min_schema_version > PR2IntegrationServiceData::MIN_SCHEMA_VERSION ||
         capability.max_schema_version < PR2IntegrationServiceData::MAX_SCHEMA_VERSION ||
         capability.request_data_token != PR2IntegrationServiceData::DATA_TOKEN ||
         capability.min_request_schema_version != PR2IntegrationServiceData::MIN_SCHEMA_VERSION ||
         capability.max_request_schema_version != PR2IntegrationServiceData::MAX_SCHEMA_VERSION ||
         capability.max_atomic_request_bits_by_vn.empty() ||
         capability.max_atomic_request_bits_by_vn[0] < 64 ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration networkIF reported an invalid capability\n");
    }
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
