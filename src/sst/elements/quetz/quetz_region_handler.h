// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

/**
 * quetz_region_handler.h — SubComponent API for guest address-range policy.
 *
 * Each handler owns an inclusive [start, end] range.  The pipeline filter
 * stage dispatches READ/WRITE commands to the first matching handler.
 */

#ifndef _H_SST_QUETZ_REGION_HANDLER
#define _H_SST_QUETZ_REGION_HANDLER

#include <sst/core/output.h>
#include <sst/core/subcomponent.h>

#include <stdint.h>

#include "quetz_shmem.h"
#include "quetz_stats.h"

namespace SST {
namespace Quetz {

class MemRegionHandler : public SST::SubComponent {
public:
    SST_ELI_REGISTER_SUBCOMPONENT_API(SST::Quetz::MemRegionHandler)

    enum class Action {
        FORWARD,  // pass through to memHierarchy (default path)
        CONSUME,  // handled locally; do not issue StandardMem
    };

    MemRegionHandler(ComponentId_t id, Params& params)
        : SST::SubComponent(id)
    {}

    virtual uint64_t startAddr() const = 0;
    virtual uint64_t endAddr()   const = 0;

    virtual Action onRead(const QuetzCommand& cmd, QuetzCoreStats& stats) = 0;
    virtual Action onWrite(const QuetzCommand& cmd, QuetzCoreStats& stats) = 0;

    virtual void finish(SST::Output* out, uint32_t core_id) {}
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_REGION_HANDLER
