// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_MERLIN_TEST_NETWORK_SERVICE_PR2_INTEGRATION_FIXTURE_H
#define SST_ELEMENTS_MERLIN_TEST_NETWORK_SERVICE_PR2_INTEGRATION_FIXTURE_H

#include "../../networkService.h"

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/link.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace SST::Merlin {

inline constexpr SST::Interfaces::SimpleNetwork::NetworkServiceID PR2_INTEGRATION_SERVICE_ID =
    SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_PLUGIN_MIN;

enum class PR2IntegrationAction : uint8_t { Pass = 1, AcceptEcho = 2, BusyUntilEcho = 3, SyntheticEcho = 4 };

/** Test-owned, non-collective sidecar used by the Merlin network-service fixtures. */
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

/** Exercises service rejection and ordinary traffic on effective VN remappings. */
class PR2VNRemapEndpoint final : public SST::Component
{
public:
    SST_ELI_REGISTER_COMPONENT(PR2VNRemapEndpoint, "merlin", "network_service_vn_remap_endpoint",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Generic network-service VN remapping probe",
        COMPONENT_CATEGORY_NETWORK)

    SST_ELI_DOCUMENT_PARAMS(
        { "expected_map", "Three effective logical-to-router VN mappings", "" }
    )
    SST_ELI_DOCUMENT_PORTS()
    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "networkIF", "Merlin network interface", "SST::Interfaces::SimpleNetwork" }
    )

    PR2VNRemapEndpoint(ComponentId_t id, Params& params);
    void init(unsigned int phase) override;
    void setup() override;
    void complete(unsigned int phase) override;
    void finish() override;

private:
    using SimpleNetwork = SST::Interfaces::SimpleNetwork;
    bool handleSend(int vn);
    bool handleReceive(int vn);
    void handleDeadline(SST::Event* event);

    SimpleNetwork* network_ = nullptr;
    SST::Link* deadline_ = nullptr;
    std::vector<int> expected_map_;
    std::vector<std::unique_ptr<SimpleNetwork::Request>> pending_;
    size_t next_to_send_ = 0;
    std::array<unsigned, 3> ordinary_received_{};
    std::array<unsigned, 3> tagged_received_{};
};

} // namespace SST::Merlin

#endif // SST_ELEMENTS_MERLIN_TEST_NETWORK_SERVICE_PR2_INTEGRATION_FIXTURE_H
