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

#ifndef _H_SST_QUETZ_IRQ_EVENT
#define _H_SST_QUETZ_IRQ_EVENT

#include <sst/core/event.h>

#include <stdint.h>

namespace SST {
namespace Quetz {

// IRQ level change from an SST MMIO device to the guest machine: sent on a
// device's "irq" port to a QuetzCPU irq_link_%d port, which forwards it to the
// QEMU bridge through the shared-memory IRQ mailbox (see quetz_ipc_types.h).
// Level semantics: the device raises (level=1) on its event of interest and
// lowers (level=0) only when the guest acks it via the device's MMIO ack
// register.
class QuetzIrqEvent : public SST::Event {
public:
    QuetzIrqEvent() : Event(), vcpu(0), line(0), level(0) {}
    QuetzIrqEvent(uint32_t vcpu, uint32_t line, uint32_t level)
        : Event(), vcpu(vcpu), line(line), level(level) {}

    uint32_t vcpu;   // target vCPU's IRQ-slot row (single-core guests: 0)
    uint32_t line;   // machine interrupt-controller input number
    uint32_t level;  // 1 = raise, 0 = lower

    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        Event::serialize_order(ser);
        SST_SER(vcpu);
        SST_SER(line);
        SST_SER(level);
    }

    ImplementSerializable(SST::Quetz::QuetzIrqEvent);
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_IRQ_EVENT
