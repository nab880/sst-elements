// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include <mercury/components/nic_api.h>

namespace SST::Hg {

VirtualNetworkConfig
resolveVirtualNetworkConfig(const SST::Params& params)
{
  VirtualNetworkConfig config;
  config.count = params.find<int>("num_vns", 1);
  config.ordinary = params.find<int>("ordinary_vn", 0);
  config.manager = params.find<int>(
      "manager_vn", config.count == 1 ? config.ordinary : -1);
  config.reduce = params.find<int>("reduce_vn", -1);
  config.result = params.find<int>("result_vn", -1);
  return config;
}

VirtualNetworkConfigProvider::~VirtualNetworkConfigProvider() = default;

CollectiveEndpointProvider::~CollectiveEndpointProvider() = default;

NicAPI::NicAPI(uint32_t id, SST::Params& params) :
  SST::Hg::SubComponent(id) { }

SST::Collective::CollectiveEndpoint*
NicAPI::collectiveEndpoint() const
{
  auto* provider = dynamic_cast<const CollectiveEndpointProvider*>(this);
  return provider ? provider->collectiveEndpoint() : nullptr;
}

bool
NicAPI::configureVirtualNetworks(const VirtualNetworkConfig& config)
{
  auto* provider = dynamic_cast<VirtualNetworkConfigProvider*>(this);
  if (provider) return provider->configureVirtualNetworks(config);

  return config.count == 1 && config.ordinary == 0 &&
         (config.manager == -1 || config.manager == 0) &&
         config.reduce == -1 && config.result == -1;
}

}
