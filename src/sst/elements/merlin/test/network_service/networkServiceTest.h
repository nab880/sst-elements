// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_MERLIN_NETWORK_SERVICE_TEST_H
#define SST_ELEMENTS_MERLIN_NETWORK_SERVICE_TEST_H

#include <sst/core/component.h>

namespace SST::Merlin {

class NetworkServiceTest final : public SST::Component
{
public:
    SST_ELI_REGISTER_COMPONENT(NetworkServiceTest, "merlin", "network_service_test",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Generic Merlin network-service transaction contract test",
        COMPONENT_CATEGORY_UNCATEGORIZED)

    SST_ELI_DOCUMENT_PARAMS()

    NetworkServiceTest(SST::ComponentId_t id, SST::Params& params);
};

} // namespace SST::Merlin

#endif // SST_ELEMENTS_MERLIN_NETWORK_SERVICE_TEST_H
