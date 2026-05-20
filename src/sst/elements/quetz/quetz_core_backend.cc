// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include "quetz_core_backend.h"

using namespace SST::Quetz;

QuetzTunnelBackend::QuetzTunnelBackend(QuetzTunnel* tunnel)
    : tunnel_(tunnel)
{}

bool QuetzTunnelBackend::readCommandNB(uint32_t core_id, QuetzCommand* cmd) {
    return tunnel_->readMessageNB((size_t)core_id, cmd);
}

void QuetzTunnelBackend::updateSimTime(uint64_t sim_time_ns) {
    tunnel_->updateTime(sim_time_ns);
}

void QuetzTunnelBackend::incrementCycles() {
    tunnel_->incrementCycles();
}
