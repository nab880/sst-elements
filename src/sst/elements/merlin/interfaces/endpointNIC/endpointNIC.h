// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef COMPONENTS_MERLIN_ENDPOINTNIC_H
#define COMPONENTS_MERLIN_ENDPOINTNIC_H

#include <sst/core/subcomponent.h>
#include <sst/core/event.h>
#include <sst/core/link.h>
#include <sst/core/output.h>
#include "sst/core/interfaces/simpleNetwork.h"
#include "NICPlugin.h"
#include <vector>

namespace SST {
namespace Merlin {

class EndpointNIC : public SST::Interfaces::SimpleNetwork
{
protected:
    int vns;
    SST::Interfaces::SimpleNetwork* link_control;
    Output out;
    nid_t EP_id;

    // Pipeline of NIC plugins
    std::vector<NICPlugin*> plugin_pipeline;

public:
    SST_ELI_REGISTER_SUBCOMPONENT(
        EndpointNIC,
        "merlin",
        "endpointNIC",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Endpoint NIC with pluggable functionality pipeline",
        SST::Interfaces::SimpleNetwork
    )

    SST_ELI_DOCUMENT_PARAMS(
        {"EP_id",        "Endpoint ID assigned to this NIC. Required.", ""},
        {"plugin_names", "Array of subcomponent slot names for NIC plugins to load in pipeline order.", ""},
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"networkIF", "Network interface", "SST::Interfaces::SimpleNetwork" },
        {"sourceRoutingPlugin", "NIC plugin slot for enabling source routing", "SST::Merlin::NICPlugin"}
        // If new plugin slots are added, document them here
    )

    EndpointNIC(ComponentId_t cid, Params& params, int vns);
    ~EndpointNIC() override;

    // SimpleNetwork interface methods - forward to link_control
    void init(unsigned int phase) override;
    void setup() override;
    void complete(unsigned int phase) override;
    void finish() override;

    bool send(Request* req, int vn) override;
    bool spaceToSend(int vn, int num_bits) override;
    Request* recv(int vn) override;
    bool requestToReceive(int vn) override;

    void sendUntimedData(Request* req) override;
    Request* recvUntimedData() override;

    void setNotifyOnReceive(HandlerBase* functor) override;
    void setNotifyOnSend(HandlerBase* functor) override;

    bool isNetworkInitialized() const override;
    nid_t getEndpointID() const override;
    const UnitAlgebra& getLinkBW() const override;
    std::vector<NetworkServiceID> getSupportedServices() const override;
    bool queryServiceCapability(NetworkServiceID id, NetworkServiceCapability& out) const override;

protected:
    // Virtual methods for child classes to for NIC-specific functionality
    Request* processOutgoingRequest(Request* req, int vn);
    Request* processIncomingRequest(Request* req, int vn);

    // Load plugins from parameters
    void loadPlugins(Params& params);

    // Process request through plugin pipeline
    Request* processThroughPipeline(Request* req, int vn, bool outgoing);
};

} // namespace Merlin
} // namespace SST

#endif
