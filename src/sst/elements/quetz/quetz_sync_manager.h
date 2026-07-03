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
        __atomic_fetch_add(&shared_->child_attached, 1u, __ATOMIC_RELEASE);
    }

    // Acquire pairs with announceAttach()'s release so the child's shared-memory
    // initialization is visible on weak-memory (e.g. ARM) hosts, not just x86.
    bool childAttached() const {
        return __atomic_load_n(&shared_->child_attached, __ATOMIC_ACQUIRE) != 0;
    }

    void waitForChild() {
        while (!childAttached())
            ;
    }

private:
    QuetzSharedData* shared_ = nullptr;
};

} // namespace Quetz
} // namespace SST

#endif // _SST_QUETZ_SYNC_MANAGER_H
