// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "sst_config.h"

#include "../../router.h"

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/link.h>
#include <sst/core/output.h>

#include <memory>
#include <utility>

// Minimal probe processor and endpoint used by network_service_model.py and
// Mercury's tag-first test.
namespace SST::Merlin::Test {

using SimpleNetwork = SST::Interfaces::SimpleNetwork;
constexpr SimpleNetwork::NetworkServiceID SERVICE_ID = SimpleNetwork::NETWORK_SERVICE_PLUGIN_MIN;
constexpr SimTime_t BUSY_RELEASE_NS = 4;

enum class Action : uint8_t { Pass = 1, Busy = 2 };

class ProbeData final : public SimpleNetwork::NetworkServiceData
{
public:
    static constexpr SimpleNetwork::NetworkServiceID SERVICE_ID = Test::SERVICE_ID;
    static constexpr SimpleNetwork::NetworkServiceDataToken DATA_TOKEN = 1;
    static constexpr SimpleNetwork::NetworkServiceVersion MIN_SCHEMA_VERSION = 1;
    static constexpr SimpleNetwork::NetworkServiceVersion MAX_SCHEMA_VERSION = 1;

    ProbeData() = default;
    ProbeData(Action action, uint32_t sequence) : action_(action), sequence_(sequence) {}

    SimpleNetwork::NetworkServiceID serviceID() const override { return SERVICE_ID; }
    SimpleNetwork::NetworkServiceDataToken dataToken() const override { return DATA_TOKEN; }
    SimpleNetwork::NetworkServiceVersion schemaVersion() const override { return MIN_SCHEMA_VERSION; }
    ProbeData* clone() const override { return new ProbeData(*this); }
    Action action() const { return action_; }
    uint32_t sequence() const { return sequence_; }

    void serialize_order(SST::Core::Serialization::serializer& ser) override
    {
        SST_SER(action_);
        SST_SER(sequence_);
    }

private:
    Action action_ = static_cast<Action>(0);
    uint32_t sequence_ = 0;
    ImplementSerializable(SST::Merlin::Test::ProbeData);
};

class ProbeProcessor final : public NetworkServiceProcessor
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(ProbeProcessor, "merlin", "network_service_probe_processor",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Minimal network-service integration processor",
        SST::Merlin::NetworkServiceProcessor)
    SST_ELI_DOCUMENT_PARAMS()

    ProbeProcessor(ComponentId_t id, Params&, NetworkServiceHost* host) : NetworkServiceProcessor(id, host)
    {
        if ( host == nullptr ) getSimulationOutput().fatal(CALL_INFO, 1, "probe processor requires a host\n");
    }
    ProbeProcessor() = default;

    std::vector<int> ownedVNs() const override { return { 0 }; }

    NetworkServiceID getServiceID() const override { return SERVICE_ID; }
    NetworkServiceRequestContract getRequestContract() const override
    {
        return { SERVICE_ID, ProbeData::DATA_TOKEN, ProbeData::MIN_SCHEMA_VERSION,
            ProbeData::MAX_SCHEMA_VERSION };
    }

    NetworkServiceDecision inspect(const NetworkServiceIngress& ingress) const override
    {
        const auto* request = ingress.event == nullptr ? nullptr : ingress.event->inspectRequest();
        const auto* data = request == nullptr ? nullptr : request->inspectServiceDataAs<ProbeData>();
        if ( ingress.input_port < 0 || ingress.input_vn != 0 || data == nullptr ||
             data->action() != Action::Busy ) {
            return { NetworkServiceDisposition::Reject, 1 };
        }
        return { getCurrentSimTimeNano() < BUSY_RELEASE_NS ? NetworkServiceDisposition::Busy :
                                                             NetworkServiceDisposition::Accept };
    }

    // Deliver the accepted packet to its destination as a synthetic packet
    // with identical service data, proving consume/re-emit conserves it.
    void consume(NetworkServiceOwnedIngress ingress) noexcept override
    {
        const auto* request = ingress.event == nullptr ? nullptr : ingress.event->inspectRequest();
        const auto* data = request == nullptr ? nullptr : request->inspectServiceDataAs<ProbeData>();
        if ( data == nullptr ) getSimulationOutput().fatal(CALL_INFO, 1, "probe consumed an invalid packet\n");
        auto echo = std::make_unique<SimpleNetwork::Request>(
            request->dest, request->src, request->size_in_bits, true, true);
        echo->vn = request->vn;
        echo->giveServiceData(data->clone());
        NetworkServiceSyntheticPacket packet;
        packet.trusted_src   = request->src;
        packet.route_vn      = ingress.event->getVN();
        packet.output_port   = static_cast<int>(request->dest);
        packet.request       = std::move(echo);
        if ( !host()->tryEnqueueNetworkServiceOutput(SERVICE_ID, packet) ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "probe could not enqueue its synthetic echo\n");
        }
    }
    bool hasScheduledWork() const override { return false; }
};

class ProbeEndpoint final : public Component
{
public:
    SST_ELI_REGISTER_COMPONENT(ProbeEndpoint, "merlin", "network_service_probe_endpoint",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Minimal network-service endpoint",
        COMPONENT_CATEGORY_NETWORK)
    SST_ELI_DOCUMENT_PARAMS({ "id", "Endpoint ID (zero or one)", "-1" })
    SST_ELI_DOCUMENT_PORTS()
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "networkIF", "Merlin network interface", "SST::Interfaces::SimpleNetwork" })

    ProbeEndpoint(ComponentId_t id, Params& params) : Component(id), id_(params.find<int>("id", -1))
    {
        network_ = loadUserSubComponent<SimpleNetwork>("networkIF", ComponentInfo::SHARE_NONE, 2);
        if ( network_ == nullptr || (id_ != 0 && id_ != 1) ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "invalid network-service probe endpoint\n");
        }
        deadline_ = configureSelfLink("deadline", "20ns",
            new Event::Handler<ProbeEndpoint, &ProbeEndpoint::deadline>(this));
        registerAsPrimaryComponent();
        primaryComponentDoNotEndSim();
    }

    void init(unsigned phase) override { network_->init(phase); }
    void complete(unsigned phase) override { network_->complete(phase); }
    void finish() override { network_->finish(); }

    void setup() override
    {
        network_->setup();
        SimpleNetwork::NetworkServiceCapability capability;
        if ( !network_->queryServiceCapability(SERVICE_ID, capability) ||
             !capability.isValidFor(SERVICE_ID) ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "probe service was not advertised\n");
        }
        network_->setNotifyOnReceive(
            new SimpleNetwork::Handler<ProbeEndpoint, &ProbeEndpoint::receive>(this));
        // Endpoint 0 sends on the processor-owned VN 0 and is held until the
        // release time; endpoint 1 sends on the unowned VN 1 and routes normally.
        const int vn = id_ == 0 ? 0 : 1;
        auto request = std::make_unique<SimpleNetwork::Request>(1 - id_, id_, 64, true, true);
        request->vn = vn;
        request->giveServiceData(new ProbeData(id_ == 0 ? Action::Busy : Action::Pass, id_));
        if ( !network_->send(request.get(), vn) ) {
            getSimulationOutput().fatal(CALL_INFO, 1, "probe injection unexpectedly blocked\n");
        }
        request.release();
        deadline_->send(1, nullptr);
    }

private:
    bool receive(int vn)
    {
        while ( network_->requestToReceive(vn) ) {
            std::unique_ptr<SimpleNetwork::Request> request(network_->recv(vn));
            const auto* data = request == nullptr ? nullptr : request->inspectServiceDataAs<ProbeData>();
            const Action expected = id_ == 0 ? Action::Pass : Action::Busy;
            const uint32_t sequence = static_cast<uint32_t>(1 - id_);
            const int expected_vn = id_ == 0 ? 1 : 0;
            if ( vn != expected_vn || data == nullptr || data->action() != expected ||
                 data->sequence() != sequence || request->src != 1 - id_ || request->dest != id_ ||
                 request->vn != expected_vn || request->size_in_bits != 64 || received_ ) {
                getSimulationOutput().fatal(CALL_INFO, 1, "tagged PASS/BUSY packet changed in transit\n");
            }
            received_ = true;
            if ( id_ == 1 ) getSimulationOutput().output("Merlin network-service integration PASS\n");
            // Endpoint 1 receives only after the processor released its Busy hold.
            if ( id_ == 1 && getCurrentSimTimeNano() < BUSY_RELEASE_NS ) {
                getSimulationOutput().fatal(CALL_INFO, 1, "Busy head was delivered before release\n");
            }
            primaryComponentOKToEndSim();
        }
        return true;
    }

    void deadline(Event* event)
    {
        delete event;
        if ( !received_ ) getSimulationOutput().fatal(CALL_INFO, 1, "network-service probe timed out\n");
    }

    int id_ = -1;
    SimpleNetwork* network_ = nullptr;
    Link* deadline_ = nullptr;
    bool received_ = false;
};

} // namespace SST::Merlin::Test
