// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "quetz_core_backend.h"

using namespace SST::Quetz;

QuetzTunnelBackend::QuetzTunnelBackend(QuetzTunnel* tunnel)
    : tunnel_(tunnel)
{}

bool QuetzTunnelBackend::readCommandNB(uint32_t core_id, QuetzCommand* cmd)
{
    if (core_id >= QUETZ_MAX_MMIO_VCORES)
        return false;

    if (!deferred_[core_id].empty()) {
        *cmd = deferred_[core_id].front();
        deferred_[core_id].pop_front();
        return true;
    }

    while (tunnel_->readMessageNB((size_t)core_id, cmd)) {
        return true;
    }
    return false;
}

void QuetzTunnelBackend::updateSimTime(uint64_t sim_time_ns) {
    tunnel_->updateTime(sim_time_ns);
}

void QuetzTunnelBackend::incrementCycles() {
    tunnel_->incrementCycles();
}
