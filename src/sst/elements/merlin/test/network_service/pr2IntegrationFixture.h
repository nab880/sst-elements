// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_MERLIN_TEST_NETWORK_SERVICE_PR2_INTEGRATION_FIXTURE_H
#define SST_ELEMENTS_MERLIN_TEST_NETWORK_SERVICE_PR2_INTEGRATION_FIXTURE_H

#include "sst/elements/merlin/networkService.h"

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/link.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace SST::Merlin {

inline constexpr SST::Interfaces::SimpleNetwork::NetworkServiceID PR2_INTEGRATION_SERVICE_ID =
    SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_PLUGIN_MIN;

enum class PR2IntegrationAction : uint8_t { Pass = 1, AcceptEcho = 2, BusyOnceEcho = 3, SyntheticEcho = 4 };

/** Test-owned, non-collective sidecar used by the PR2 Merlin integration fixture. */
class PR2IntegrationServiceData final : public SST::Interfaces::SimpleNetwork::NetworkServiceData
{
public:
    using SimpleNetwork = SST::Interfaces::SimpleNetwork;

    static constexpr SimpleNetwork::NetworkServiceID SERVICE_ID = PR2_INTEGRATION_SERVICE_ID;
    static constexpr SimpleNetwork::NetworkServiceDataToken DATA_TOKEN = 1;
    static constexpr SimpleNetwork::NetworkServiceVersion MIN_SCHEMA_VERSION = 1;
    static constexpr SimpleNetwork::NetworkServiceVersion MAX_SCHEMA_VERSION = 1;

    PR2IntegrationServiceData() = default;
    PR2IntegrationServiceData(PR2IntegrationAction action, uint32_t sequence) :
        action_(action),
        sequence_(sequence)
    {}

    SimpleNetwork::NetworkServiceID serviceID() const override { return SERVICE_ID; }
    SimpleNetwork::NetworkServiceDataToken dataToken() const override { return DATA_TOKEN; }
    SimpleNetwork::NetworkServiceVersion schemaVersion() const override { return MIN_SCHEMA_VERSION; }
    PR2IntegrationServiceData* clone() const override { return new PR2IntegrationServiceData(*this); }

    PR2IntegrationAction action() const { return action_; }
    uint32_t sequence() const { return sequence_; }

    void serialize_order(SST::Core::Serialization::serializer& ser) override;

private:
    PR2IntegrationAction action_ = static_cast<PR2IntegrationAction>(0);
    uint32_t sequence_ = 0;

    ImplementSerializable(SST::Merlin::PR2IntegrationServiceData);
};

/** Router-side fixture: PASSes one action and ACCEPTs another to emit a fresh echo. */
class PR2IntegrationProcessor final : public NetworkServiceProcessor
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(PR2IntegrationProcessor, "merlin", "network_service_pr2_processor",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Generic PR2 network-service integration processor",
        SST::Merlin::NetworkServiceProcessor)

    SST_ELI_DOCUMENT_PARAMS()

    SST_ELI_DOCUMENT_PORTS(
        { "trigger", "External trigger used to prove synthetic wakeup after router declocking", { "SST::Event" } }
    )

    PR2IntegrationProcessor(ComponentId_t id, Params& params, NetworkServiceHost* host);

    NetworkServiceID getServiceID() const override { return PR2_INTEGRATION_SERVICE_ID; }
    NetworkServicePrepared prepare(const NetworkServiceIngress& ingress) override;
    bool hasScheduledWork() const override { return false; }

private:
    class Reservation;

    void emitEcho(std::unique_ptr<internal_router_event> event, uint32_t sequence) noexcept;
    void handleTrigger(SST::Event* event);

    NetworkServiceHost* host_ = nullptr;
    SST::Link* trigger_ = nullptr;
    bool busy_once_seen_ = false;
};

/** Two-node endpoint driver for the real single-router integration test. */
class PR2IntegrationEndpoint final : public SST::Component
{
public:
    SST_ELI_REGISTER_COMPONENT(PR2IntegrationEndpoint, "merlin", "network_service_pr2_endpoint",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Endpoint for the PR2 network-service integration fixture",
        COMPONENT_CATEGORY_NETWORK)

    SST_ELI_DOCUMENT_PARAMS(
        { "id", "Endpoint ID; this fixture requires IDs 0 and 1", "-1" }
    )

    SST_ELI_DOCUMENT_PORTS(
        { "service_trigger", "External trigger link to the test processor", { "SST::Event" } }
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "networkIF", "Merlin network interface", "SST::Interfaces::SimpleNetwork" }
    )

    PR2IntegrationEndpoint(ComponentId_t id, Params& params);
    ~PR2IntegrationEndpoint() override = default;

    void init(unsigned int phase) override;
    void setup() override;
    void complete(unsigned int phase) override;
    void finish() override;

private:
    using SimpleNetwork = SST::Interfaces::SimpleNetwork;

    bool handleReceive(int vn);
    bool handleSend(int vn);
    void handleDrain(SST::Event* event);
    void handleTriggerTimer(SST::Event* event);
    void handleDeadline(SST::Event* event);
    std::unique_ptr<SimpleNetwork::Request> makeRequest(
        PR2IntegrationAction action, uint32_t sequence, SimpleNetwork::nid_t destination) const;
    void validateCapability() const;

    int endpoint_id_ = -1;
    SimpleNetwork* network_ = nullptr;
    SST::Link* deadline_ = nullptr;
    SST::Link* drain_ = nullptr;
    SST::Link* trigger_ = nullptr;
    SST::Link* trigger_timer_ = nullptr;
    std::array<std::unique_ptr<SimpleNetwork::Request>, 3> pending_;
    size_t next_to_send_ = 0;
    uint32_t sent_ = 0;
    uint32_t pass_received_ = 0;
    uint32_t echo_received_ = 0;
    uint32_t echo_sequence_mask_ = 0;
    uint32_t pre_drain_notifications_ = 0;
    bool drain_enabled_ = false;
};

/** Verifies that endpoint configuration cannot advertise a missing router processor. */
class PR2MissingProcessorEndpoint final : public SST::Component
{
public:
    SST_ELI_REGISTER_COMPONENT(PR2MissingProcessorEndpoint, "merlin",
        "network_service_missing_processor_endpoint", SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Endpoint probe for missing Merlin network-service processors", COMPONENT_CATEGORY_NETWORK)

    SST_ELI_DOCUMENT_PARAMS(
        { "expect_processor", "Require and exercise the production PASS processor", "false" }
    )
    SST_ELI_DOCUMENT_PORTS()
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "networkIF", "Merlin network interface", "SST::Interfaces::SimpleNetwork" }
    )

    PR2MissingProcessorEndpoint(ComponentId_t id, Params& params);

    void init(unsigned int phase) override;
    void setup() override;
    void complete(unsigned int phase) override;
    void finish() override;

private:
    bool handleReceive(int vn);

    SST::Interfaces::SimpleNetwork* network_ = nullptr;
    bool expect_processor_ = false;
};

} // namespace SST::Merlin

#endif // SST_ELEMENTS_MERLIN_TEST_NETWORK_SERVICE_PR2_INTEGRATION_FIXTURE_H
