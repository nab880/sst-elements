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

class PR2IntegrationProcessor::Reservation final : public NetworkServiceReservation
{
public:
    Reservation(PR2IntegrationProcessor* processor, uint32_t sequence) :
        processor_(processor),
        sequence_(sequence)
    {}

    void commit(std::unique_ptr<internal_router_event> event) noexcept override
    {
        PR2IntegrationProcessor* processor = processor_;
        processor_ = nullptr;
        if ( processor != nullptr ) processor->emitEcho(std::move(event), sequence_);
    }

    void rollback() noexcept override { processor_ = nullptr; }

private:
    PR2IntegrationProcessor* processor_ = nullptr;
    uint32_t sequence_ = 0;
};

PR2IntegrationProcessor::PR2IntegrationProcessor(
    ComponentId_t id, Params&, NetworkServiceHost* host) :
    NetworkServiceProcessor(id),
    host_(host)
{
    if ( host_ == nullptr ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration processor requires a network-service host\n");
    }
    trigger_ = configureLink("trigger",
        new SST::Event::Handler<PR2IntegrationProcessor, &PR2IntegrationProcessor::handleTrigger>(this));
    if ( trigger_ == nullptr ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration processor requires its trigger port\n");
    }
}

NetworkServicePrepared
PR2IntegrationProcessor::prepare(const NetworkServiceIngress& ingress)
{
    if ( !ingress.head.valid() || ingress.event == nullptr || ingress.event != ingress.head.event ) {
        return { NetworkServiceDisposition::Reject, {}, 1 };
    }

    const auto* request = ingress.event->inspectRequest();
    const auto* data = request == nullptr ? nullptr : request->inspectServiceDataAs<PR2IntegrationServiceData>();
    if ( data == nullptr ) return { NetworkServiceDisposition::Reject, {}, 2 };

    switch ( data->action() ) {
    case PR2IntegrationAction::Pass:
        if ( data->sequence() == 1 &&
             (!busy_once_seen_ || getCurrentSimTimeNano() != busy_probe_time_) ) {
            return { NetworkServiceDisposition::Reject, {}, 5 };
        }
        return { NetworkServiceDisposition::Pass };
    case PR2IntegrationAction::AcceptEcho:
        return { NetworkServiceDisposition::Accept, std::make_unique<Reservation>(this, data->sequence()) };
    case PR2IntegrationAction::BusyOnceEcho:
        if ( !busy_once_seen_ ) {
            busy_once_seen_ = true;
            busy_probe_time_ = getCurrentSimTimeNano();
            return { NetworkServiceDisposition::Busy };
        }
        return { NetworkServiceDisposition::Accept, std::make_unique<Reservation>(this, data->sequence()) };
    case PR2IntegrationAction::SyntheticEcho:
        return { NetworkServiceDisposition::Reject, {}, 3 };
    }
    return { NetworkServiceDisposition::Reject, {}, 4 };
}

void
PR2IntegrationProcessor::handleTrigger(SST::Event* event)
{
    delete event;
    auto* router = dynamic_cast<hr_router*>(host_);
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
    packet.output_vc = 0;
    packet.size_in_flits = 1;
    if ( !host_->tryEnqueueNetworkServiceOutput(PR2_INTEGRATION_SERVICE_ID, packet) ) {
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
          original_data->action() != PR2IntegrationAction::BusyOnceEcho) ||
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
    packet.output_vc = event->getVC();
    packet.size_in_flits = event->getFlitCount();
    packet.request = std::move(echo);

    if ( !host_->tryEnqueueNetworkServiceOutput(PR2_INTEGRATION_SERVICE_ID, packet) ) {
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

    network_ = loadUserSubComponent<SimpleNetwork>("networkIF", ComponentInfo::SHARE_NONE, 1);
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
        pending_[0] = makeRequest(PR2IntegrationAction::BusyOnceEcho, 3, 1);
        pending_[1] = makeRequest(PR2IntegrationAction::AcceptEcho, 2, 1);
        pending_count_ = 2;
        trigger_timer_->send(1, nullptr);
        drain_->send(1, nullptr);
    }
    else {
        pending_[0] = makeRequest(PR2IntegrationAction::Pass, 1, 0);
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
    if ( vn != 0 ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration endpoint received an invalid send notification\n");
    }

    while ( next_to_send_ < pending_count_ ) {
        auto& request = pending_[next_to_send_];
        if ( !network_->send(request.get(), 0) ) return true;
        request.release();
        ++next_to_send_;
        ++sent_;
    }
    return false;
}

bool
PR2IntegrationEndpoint::handleReceive(int vn)
{
    if ( vn != 0 ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration endpoint received on an invalid VN\n");
    }

    if ( endpoint_id_ == 0 && !drain_enabled_ ) {
        if ( ++pre_drain_notifications_ > 1 ) {
            getSimulationOutput().fatal(CALL_INFO, 1,
                "PR2 integration endpoint exceeded one credited receive before its drain\n");
        }
        return true;
    }

    while ( network_->requestToReceive(vn) ) {
        std::unique_ptr<SimpleNetwork::Request> request(network_->recv(vn));
        if ( request == nullptr ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration endpoint observed an empty receive queue head\n");
        }

        const auto* data = request->inspectServiceDataAs<PR2IntegrationServiceData>();
        if ( data == nullptr || request->size_in_bits != 64 || request->vn != 0 ||
             !request->head || !request->tail ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration endpoint received malformed service data\n");
        }

        if ( endpoint_id_ == 0 && data->action() == PR2IntegrationAction::Pass &&
             data->sequence() == 1 && request->src == 1 && request->dest == 0 ) {
            ++pass_received_;
        }
        else if ( endpoint_id_ == 0 && data->action() == PR2IntegrationAction::SyntheticEcho &&
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
    if ( pre_drain_notifications_ != 1 ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "PR2 integration endpoint expected exactly one credited receive before its drain\n");
    }
    drain_enabled_ = true;
    handleReceive(0);
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
    PR2IntegrationAction action, uint32_t sequence, SimpleNetwork::nid_t destination) const
{
    auto request = std::make_unique<SimpleNetwork::Request>(destination, endpoint_id_, 64, true, true);
    request->vn = 0;
    request->giveServiceData(new PR2IntegrationServiceData(action, sequence));
    return request;
}

void
PR2IntegrationEndpoint::validateCapability() const
{
    const auto services = network_->getSupportedServices();
    if ( !std::binary_search(services.begin(), services.end(), PR2_INTEGRATION_SERVICE_ID) ) {
        getSimulationOutput().fatal(CALL_INFO, 1, "PR2 integration networkIF did not advertise the test service\n");
    }

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
         capability.max_atomic_request_bits_by_vn[0] < 64 ||
         (capability.features & SimpleNetwork::SERVICE_FEATURE_CHECKPOINT_SAFE_CARRIAGE) ) {
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
        const auto services = network_->getSupportedServices();
        SST::Interfaces::SimpleNetwork::NetworkServiceCapability capability;
        if ( !std::binary_search(services.begin(), services.end(), PR2_INTEGRATION_SERVICE_ID) ||
             !network_->queryServiceCapability(PR2_INTEGRATION_SERVICE_ID, capability) ||
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

    if ( !network_->getSupportedServices().empty() ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "Missing router processor was advertised as a network service\n");
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

} // namespace SST::Merlin
