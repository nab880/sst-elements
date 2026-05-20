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

/**
 * quetz_core_backend.h — abstract per-vCPU command source for QuetzCore.
 *
 * QuetzCore drains guest instruction and memory events through a
 * QuetzCoreBackend implementation.  The QEMU plugin writes into a shared
 * memory tunnel today; a future PIN or trace-replay backend would provide
 * the same QuetzCommand stream through a different transport.
 */

#ifndef _H_SST_QUETZ_CORE_BACKEND
#define _H_SST_QUETZ_CORE_BACKEND

#include <stdint.h>

#include "quetz_shmem.h"

namespace SST {
namespace Quetz {

class QuetzCoreBackend {
public:
    virtual ~QuetzCoreBackend() = default;

    /** Non-blocking read of the next command for @p core_id. */
    virtual bool readCommandNB(uint32_t core_id, QuetzCommand* cmd) = 0;

    /** Publish current SST simulation time to the backend (back-pressure). */
    virtual void updateSimTime(uint64_t sim_time_ns) = 0;

    /** Increment the per-tick cycle counter visible to the backend. */
    virtual void incrementCycles() = 0;
};

class QuetzTunnelBackend : public QuetzCoreBackend {
public:
    explicit QuetzTunnelBackend(QuetzTunnel* tunnel);

    bool readCommandNB(uint32_t core_id, QuetzCommand* cmd) override;
    void updateSimTime(uint64_t sim_time_ns) override;
    void incrementCycles() override;

private:
    QuetzTunnel* tunnel_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_CORE_BACKEND
