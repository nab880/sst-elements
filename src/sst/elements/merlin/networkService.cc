// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "networkService.h"
#include "networkServicePass.h"

#include <sst/core/output.h>
#include <sst/core/params.h>

namespace SST::Merlin {

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

} // namespace SST::Merlin
