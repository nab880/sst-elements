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
 * quetz_sync_manager.h — cross-process attach handshake.
 */

#ifndef _SST_QUETZ_SYNC_MANAGER_H
#define _SST_QUETZ_SYNC_MANAGER_H

#include "quetz_ipc_types.h"

namespace SST {
namespace Quetz {

class QuetzSyncManager {
public:
    void bind(QuetzSharedData* shared) { shared_ = shared; }

    void initMaster(size_t num_cores) {
        shared_->numCores       = num_cores;
        shared_->child_attached = 0;
    }

    void announceAttach() {
        __sync_fetch_and_add(&shared_->child_attached, 1u);
    }

    void waitForChild() {
        while (shared_->child_attached == 0)
            __sync_synchronize();
    }

private:
    QuetzSharedData* shared_ = nullptr;
};

} // namespace Quetz
} // namespace SST

#endif // _SST_QUETZ_SYNC_MANAGER_H
