// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "networkService.h"

#include "router.h"

#include <sst/core/output.h>
#include <sst/core/params.h>

namespace SST::Merlin {

NetworkServiceTakeResult
takeNetworkServiceIngressExpected(NetworkServiceIngressQueue& queue, int input_port, int input_vc,
    const internal_router_event* expected, NetworkServiceOwnedIngress& ingress) noexcept
{
    if ( expected == nullptr || input_port < 0 || input_vc < 0 || ingress.event ) {
        return NetworkServiceTakeResult::InvalidExpected;
    }
    std::unique_ptr<internal_router_event> event(
        queue.recvNetworkServiceExpected(input_vc, expected));
    if ( !event ) return NetworkServiceTakeResult::HeadChanged;
    if ( event.get() != expected ) return NetworkServiceTakeResult::InvalidExpected;
    ingress.input_port = input_port;
    ingress.input_vc = input_vc;
    ingress.event = std::move(event);
    return NetworkServiceTakeResult::Taken;
}

bool
NetworkServiceSyntheticPacket::valid(NetworkServiceID service_id) const
{
    return service_id != SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_NONE && request &&
           request->getServiceID() == service_id && request->src >= 0 && request->dest >= 0 && request->vn >= 0 &&
           trusted_src >= 0 && route_vn >= 0 && output_port >= 0 && output_vc >= 0 && size_in_flits > 0 &&
           request->size_in_bits > 0;
}

NetworkServicePassProcessor::NetworkServicePassProcessor(
    ComponentId_t id, Params& params, NetworkServiceHost* host) :
    NetworkServiceProcessor(id)
{
    const uint32_t configured_id = params.find<uint32_t>("service_id", 0);
    if ( host == nullptr || configured_id == SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_NONE ||
         configured_id > SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_PLUGIN_MAX ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "merlin.network_service_pass requires a host and a nonzero 16-bit service_id\n");
    }
    service_id_ = static_cast<NetworkServiceID>(configured_id);
}

NetworkServiceDecision
NetworkServicePassProcessor::inspect(const NetworkServiceIngress& ingress) const
{
    if ( ingress.input_port < 0 || ingress.input_vc < 0 || ingress.event == nullptr ) {
        return { NetworkServiceDisposition::Reject, 1 };
    }
    const auto* request = ingress.event->inspectRequest();
    if ( request == nullptr || request->getServiceID() != service_id_ ) {
        return { NetworkServiceDisposition::Reject, 2 };
    }
    return { NetworkServiceDisposition::Pass };
}

} // namespace SST::Merlin
