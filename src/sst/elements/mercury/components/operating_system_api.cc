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

#include <mercury/components/operating_system_api.h>
#include <mercury/components/node_base.h>

namespace SST {
namespace Hg {

OperatingSystemAPI::OperatingSystemAPI(ComponentId_t id, SST::Params& params)
: SST::Hg::SubComponent(id) {}

SST::Collective::CollectiveEndpoint*
OperatingSystemAPI::collectiveEndpoint() const
{
  NodeBase* parent = node();
  NicAPI* nic = parent ? parent->nic() : nullptr;
  return nic ? nic->collectiveEndpoint() : nullptr;
}

} // namespace Hg
} // namespace SST
