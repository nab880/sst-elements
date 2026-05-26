// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef _SST_QUETZ_MMIO_SYNC_H
#define _SST_QUETZ_MMIO_SYNC_H

#include "quetz_ipc_types.h"

namespace SST {
namespace Quetz {

class QuetzMmioSync {
public:
    void bind(QuetzSharedData* shared) { shared_ = shared; }

    void clearSlot(uint32_t vcpu) {
        if (vcpu >= QUETZ_MAX_MMIO_VCORES || !shared_)
            return;
        shared_->mmio_slot[vcpu].ready = 0;
        shared_->mmio_slot[vcpu].value = 0;
    }

    void postResponse(uint32_t vcpu, uint64_t value) {
        if (vcpu >= QUETZ_MAX_MMIO_VCORES || !shared_)
            return;
        shared_->mmio_slot[vcpu].value = value;
        __sync_synchronize();
        shared_->mmio_slot[vcpu].ready = 1;
    }

    void waitResponse(uint32_t vcpu, uint64_t* value_out) {
        if (vcpu >= QUETZ_MAX_MMIO_VCORES || !shared_ || !value_out)
            return;
        while (shared_->mmio_slot[vcpu].ready == 0)
            __sync_synchronize();
        *value_out = shared_->mmio_slot[vcpu].value;
        shared_->mmio_slot[vcpu].ready = 0;
        __sync_synchronize();
    }

private:
    QuetzSharedData* shared_ = nullptr;
};

} // namespace Quetz
} // namespace SST

#endif // _SST_QUETZ_MMIO_SYNC_H
