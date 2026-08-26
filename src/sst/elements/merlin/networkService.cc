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

NetworkServiceApplyResult
applyNetworkServicePrepared(NetworkServiceIngressQueue& queue, int vc,
    const NetworkServiceHeadIdentity& expected, NetworkServicePrepared&& prepared,
    uint64_t& opaque_diagnostic) noexcept
{
    opaque_diagnostic = prepared.opaque_diagnostic;
    if ( !expected.valid() || !isValid(prepared.disposition) ) {
        if ( prepared.reservation ) prepared.reservation->rollback();
        return NetworkServiceApplyResult::InvalidPrepared;
    }

    if ( prepared.disposition != NetworkServiceDisposition::Accept && prepared.reservation ) {
        prepared.reservation->rollback();
        return NetworkServiceApplyResult::InvalidPrepared;
    }

    switch ( prepared.disposition ) {
    case NetworkServiceDisposition::Pass:
        return NetworkServiceApplyResult::Passed;
    case NetworkServiceDisposition::Busy:
        return NetworkServiceApplyResult::Busy;
    case NetworkServiceDisposition::Accept:
    {
        if ( !prepared.reservation ) return NetworkServiceApplyResult::InvalidPrepared;
        std::unique_ptr<internal_router_event> event(queue.recvNetworkServiceExpected(vc, expected));
        if ( !event || event.get() != expected.event ) {
            prepared.reservation->rollback();
            return event ? NetworkServiceApplyResult::InvalidPrepared : NetworkServiceApplyResult::HeadChanged;
        }
        prepared.reservation->commit(std::move(event));
        return NetworkServiceApplyResult::Accepted;
    }
    case NetworkServiceDisposition::Reject:
    {
        std::unique_ptr<internal_router_event> event(queue.recvNetworkServiceExpected(vc, expected));
        if ( !event ) return NetworkServiceApplyResult::HeadChanged;
        if ( event.get() != expected.event ) return NetworkServiceApplyResult::InvalidPrepared;
        return NetworkServiceApplyResult::Rejected;
    }
    }
    return NetworkServiceApplyResult::InvalidPrepared;
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

NetworkServicePrepared
NetworkServicePassProcessor::prepare(const NetworkServiceIngress& ingress)
{
    if ( !ingress.head.valid() || ingress.event == nullptr || ingress.event != ingress.head.event ) {
        return { NetworkServiceDisposition::Reject, {}, 1 };
    }
    const auto* request = ingress.event->inspectRequest();
    if ( request == nullptr || request->getServiceID() != service_id_ ) {
        return { NetworkServiceDisposition::Reject, {}, 2 };
    }
    return { NetworkServiceDisposition::Pass };
}

} // namespace SST::Merlin
