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

    void clearIrqSlots(uint32_t vcpu) {
        if (vcpu >= QUETZ_MAX_MMIO_VCORES || !shared_)
            return;
        for (unsigned l = 0; l < QUETZ_MAX_IRQ_LINES; l++) {
            shared_->irq_slot[vcpu][l].seq   = 0;
            shared_->irq_slot[vcpu][l].level = 0;
        }
    }

    // Publish an IRQ level change for the QEMU bridge's poll timer. Seqlock
    // with SST as the only writer (QuetzCPU handles irq_link events serially):
    // store the level, then release-store the bumped seq so the bridge's
    // acquire-load of seq guarantees it sees this level or a newer one.
    void postIrq(uint32_t vcpu, uint32_t line, uint32_t level) {
        if (vcpu >= QUETZ_MAX_MMIO_VCORES || line >= QUETZ_MAX_IRQ_LINES ||
            !shared_)
            return;
        QuetzIrqSlot& slot = shared_->irq_slot[vcpu][line];
        slot.level = level;
        __atomic_store_n(&slot.seq, slot.seq + 1, __ATOMIC_RELEASE);
        // Global change stamp, bumped after the slot: the bridge's poll tick
        // acquire-loads it and skips the whole irq_slot matrix scan when it
        // has not moved, so an idle run costs one load per tick instead of
        // numCores x QUETZ_MAX_IRQ_LINES.
        __atomic_store_n(&shared_->irq_generation,
                         shared_->irq_generation + 1, __ATOMIC_RELEASE);
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
