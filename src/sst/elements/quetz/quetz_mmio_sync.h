// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef _SST_QUETZ_MMIO_SYNC_H
#define _SST_QUETZ_MMIO_SYNC_H

#include "quetz_ipc_types.h"

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

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

    // `ready` is the publication flag for `value`: release-store on the producer
    // and acquire-load on the consumer so the pairing is correct on weak-memory
    // (e.g. ARM) hosts, not just x86-TSO.
    void postResponse(uint32_t vcpu, uint64_t value) {
        if (vcpu >= QUETZ_MAX_MMIO_VCORES || !shared_)
            return;
        shared_->mmio_slot[vcpu].value = value;
        __atomic_store_n(&shared_->mmio_slot[vcpu].ready, 1u, __ATOMIC_RELEASE);
#ifdef __linux__
        // Wake the guest vCPU thread blocked in the QEMU bridge's futex wait.
        // Non-Linux hosts have no futex; the bridge falls back to the spin in
        // waitResponse(), for which the release-store above is sufficient.
        syscall(SYS_futex, (uint32_t*)&shared_->mmio_slot[vcpu].ready,
                FUTEX_WAKE, 1, nullptr, nullptr, 0);
#endif
    }

    void waitResponse(uint32_t vcpu, uint64_t* value_out) {
        if (vcpu >= QUETZ_MAX_MMIO_VCORES || !shared_ || !value_out)
            return;
        while (__atomic_load_n(&shared_->mmio_slot[vcpu].ready,
                               __ATOMIC_ACQUIRE) == 0)
            ;
        *value_out = shared_->mmio_slot[vcpu].value;
        __atomic_store_n(&shared_->mmio_slot[vcpu].ready, 0u, __ATOMIC_RELEASE);
    }

private:
    QuetzSharedData* shared_ = nullptr;
};

} // namespace Quetz
} // namespace SST

#endif // _SST_QUETZ_MMIO_SYNC_H
