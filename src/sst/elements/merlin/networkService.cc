// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "networkService.h"
#include "networkServicePass.h"

#include "router.h"

#include <sst/core/output.h>
#include <sst/core/params.h>

namespace SST::Merlin {

bool
NetworkServiceSyntheticPacket::valid(NetworkServiceID service_id) const
{
    return service_id != SST::Interfaces::SimpleNetwork::NETWORK_SERVICE_NONE && request &&
           request->getServiceID() == service_id && request->src >= 0 && request->dest >= 0 && request->vn >= 0 &&
           trusted_src >= 0 && route_vn >= 0 && output_port >= 0 &&
           request->size_in_bits > 0;
}

void
NetworkServiceSyntheticPacket::serialize_order(SST::Core::Serialization::serializer& ser)
{
    SST::Interfaces::SimpleNetwork::Request* raw =
        ser.mode() == SST::Core::Serialization::serializer::UNPACK ? nullptr : request.get();
    SST_SER(raw);
    if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) request.reset(raw);
    SST_SER(trusted_src);
    SST_SER(route_vn);
    SST_SER(output_port);
}

NetworkServicePassProcessor::NetworkServicePassProcessor(
    ComponentId_t id, Params& params, NetworkServiceHost* host) :
    NetworkServiceProcessor(id, host)
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
NetworkServicePassProcessor::inspect(const NetworkServiceIngress&) const
{
    // Owns no VN, so the router never offers it a head.
    return { NetworkServiceDisposition::Reject, 1 };
}

} // namespace SST::Merlin
