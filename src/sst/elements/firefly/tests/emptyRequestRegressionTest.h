// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.

#ifndef SST_ELEMENTS_FIREFLY_TESTS_EMPTY_REQUEST_REGRESSION_TEST_H
#define SST_ELEMENTS_FIREFLY_TESTS_EMPTY_REQUEST_REGRESSION_TEST_H

#include <sst/core/component.h>
#include <sst/core/interfaces/simpleNetwork.h>
#include <sst/core/link.h>

namespace SST::Firefly {

class EmptyRequestRegressionTest final : public SST::Component
{
public:
    SST_ELI_REGISTER_COMPONENT(EmptyRequestRegressionTest, "firefly",
        "empty_request_regression_test", SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "Inject an ordinary empty SimpleNetwork Request into a Firefly NIC",
        COMPONENT_CATEGORY_NETWORK)

    SST_ELI_DOCUMENT_PARAMS(
        { "destination", "Destination endpoint NID", "1" },
        { "vn", "Ordinary virtual network", "0" },
        { "request_bits", "Modeled request size", "64" }
    )

    SST_ELI_DOCUMENT_PORTS(
        { "nic", "Firefly NIC host link used only for initialization", {} }
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "networkIF", "SimpleNetwork interface used by the injector",
            "SST::Interfaces::SimpleNetwork" }
    )

    EmptyRequestRegressionTest(ComponentId_t id, Params& params);

    void init(unsigned int phase) override;
    void setup() override;
    void finish() override;

private:
    void inject(SST::Event* event);
    void deadline(SST::Event* event);
    [[noreturn]] void fail(const char* reason) const;

    SST::Interfaces::SimpleNetwork* network_ = nullptr;
    SST::Link* nic_link_ = nullptr;
    SST::Link* inject_link_ = nullptr;
    SST::Link* deadline_link_ = nullptr;
    SST::Interfaces::SimpleNetwork::nid_t destination_ = 1;
    int vn_ = 0;
    size_t request_bits_ = 64;
    bool sent_ = false;
    bool nic_initialized_ = false;
};

} // namespace SST::Firefly

#endif // SST_ELEMENTS_FIREFLY_TESTS_EMPTY_REQUEST_REGRESSION_TEST_H
