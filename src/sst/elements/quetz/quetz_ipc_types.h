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



#ifndef _SST_QUETZ_IPC_TYPES_H
#define _SST_QUETZ_IPC_TYPES_H

#include <stddef.h>
#include <stdint.h>

namespace SST {
namespace Quetz {

enum QuetzShmemCmd : uint32_t {
    QUETZ_CMD_NOP            = 0,
    QUETZ_CMD_READ           = 1,
    QUETZ_CMD_WRITE          = 2,
    QUETZ_CMD_EXIT           = 3,
    QUETZ_CMD_MMIO_READ_REQ  = 4,
    QUETZ_CMD_MMIO_WRITE_REQ = 5,
    // Synchronous cache operation: size=0 MOVEC (addr=control register,
    // value=register value), size=1 CPUSHL (addr=An set/way, value=instruction).
    QUETZ_CMD_CACHE_OP       = 7,
    // A run of `size` consecutive non-memory insns of one `insn_class`; the SST
    // input stage re-expands it into `size` NOP events (P2 ring-traffic cut).
    QUETZ_CMD_COMPUTE_RUN    = 6,
};

// Producer flushes a compute run at this length so the count field and the
// inter-flush gap stay bounded; the consumer re-expands any size, so the exact
// value only trades ring messages against accumulator latency.
static constexpr uint32_t QUETZ_COMPUTE_RUN_MAX = 4096;

enum QuetzInsnClass : uint32_t {
    QUETZ_INSN_INT_MEM      = 0,
    QUETZ_INSN_FP_MEM       = 1,
    QUETZ_INSN_VEC_MEM      = 2,
    QUETZ_INSN_INT_COMPUTE  = 3,
    QUETZ_INSN_FP_COMPUTE   = 4,
    QUETZ_INSN_VEC_COMPUTE  = 5,
    QUETZ_INSN_BRANCH       = 6,
    QUETZ_INSN_OTHER        = 7,
    QUETZ_INSN_CLASS_COUNT  = 8
};

static constexpr unsigned QUETZ_CMD_DATA_BYTES = 64;

struct QuetzCommand {
    QuetzShmemCmd cmd;
    uint32_t      size;
    uint64_t      pc;
    uint64_t      addr;
    uint32_t      insn_class;
    uint32_t      _pad;
    uint8_t       data[QUETZ_CMD_DATA_BYTES];
};


struct QuetzMmioResponseSlot {
    volatile uint32_t ready;
    uint32_t          _pad;
    uint64_t          value;
};


struct QuetzMmioSyncRequest {
    volatile uint32_t pending;
    uint32_t          cmd;
    uint32_t          size;
    uint32_t          _pad;
    uint64_t          addr;
    uint64_t          write_val;
};

static constexpr unsigned QUETZ_MAX_MMIO_VCORES = 256;

// Reverse (SST -> guest) IRQ mailbox: one slot per (vcore, machine IRQ line).
//
// Single-writer seqlock, no handshake: SST (the only writer) stores `level`
// and then release-stores an incremented `seq`; the QEMU bridge polls with an
// acquire-load of `seq` and re-applies qemu_set_irq(level) whenever seq moved.
// QEMU never writes the slot, so there is no lost-update window — a consumer
// that pairs a stale seq with a newer level merely re-applies the same level
// on its next poll tick.
//
// CONTRACT (level semantics, not edges): the slot carries the CURRENT line
// level, and the poller only ever observes the latest value — a raise
// followed by a lower between two poll ticks collapses to the final level.
// Transient pulses are therefore lost BY DESIGN. A device must hold the line
// raised for as long as unconsumed work exists and lower it only when the
// guest has acked everything (see QuetzGpuDevice::ackIrq's event counting) —
// under that discipline the collapsed observation is always the correct one.
// Keep in sync with qemu-overlay/include/quetz/quetz_ipc_types.h.
static constexpr unsigned QUETZ_MAX_IRQ_LINES = 64;

struct QuetzIrqSlot {
    volatile uint32_t seq;    // release-store by SST, acquire-load by QEMU
    uint32_t          level;  // 1 = raise, 0 = lower
};

// Layout stamp written by the SST master at init and verified by every
// attaching client (QEMU bridge / overlay IPC client) before it touches the
// region. It sits at the END of QuetzSharedData on purpose: its offset moves
// if either this struct or SST-core's tunnel header (which the overlay client
// hand-mirrors) drifts, turning silent layout skew into a loud attach
// failure. 'QZM' + layout version — BUMP THE LOW BYTE on ANY change to this
// struct, and keep the overlay mirror header in lockstep.
static constexpr uint32_t QUETZ_SHM_MAGIC = 0x515A4D04u;
static constexpr uint32_t QUETZ_LOCAL_RAM_BYTES = 65536u;

struct QuetzSharedData {
    size_t            numCores;
    uint64_t          simTime;
    uint64_t          simCycles;
    volatile uint32_t child_attached;
    uint32_t          _pad0;
    QuetzMmioResponseSlot mmio_slot[QUETZ_MAX_MMIO_VCORES];
    QuetzMmioSyncRequest  mmio_req[QUETZ_MAX_MMIO_VCORES];
    QuetzIrqSlot          irq_slot[QUETZ_MAX_MMIO_VCORES][QUETZ_MAX_IRQ_LINES];
    // Bumped (release) by SST on every postIrq; the QEMU bridge's poll tick
    // acquire-loads it and skips the whole irq_slot scan when unchanged.
    volatile uint32_t irq_generation;
    uint32_t          _pad2;
    // QEMU publishes each CPU reset before that CPU can resume execution.
    // SST acquire-loads the epoch before consuming the CPU's next request.
    volatile uint32_t cpu_reset_epoch[QUETZ_MAX_MMIO_VCORES];
    uint32_t local_ram_offset;
    // QEMU maps aligned P1/P2 RAM directly from this backing. CPU data uses
    // the same per-core cache as the SST window; ELF loading, fetch and DMA
    // use these bytes directly. Extra space permits 64-KiB pointer alignment.
    uint8_t local_ram_storage[3 * QUETZ_LOCAL_RAM_BYTES - 1];
    volatile uint32_t magic;   // QUETZ_SHM_MAGIC — keep as the LAST field
    uint32_t          _pad1;
};

inline uint8_t* quetzLocalRam(QuetzSharedData* shared, unsigned bank) {
    const size_t begin = offsetof(QuetzSharedData, local_ram_storage);
    const size_t end = begin + sizeof(shared->local_ram_storage);
    if (bank >= 2 || shared->local_ram_offset < begin ||
        shared->local_ram_offset > end - 2 * QUETZ_LOCAL_RAM_BYTES)
        return nullptr;
    return reinterpret_cast<uint8_t*>(shared) + shared->local_ram_offset +
        bank * QUETZ_LOCAL_RAM_BYTES;
}

inline void quetzInitializeLocalRam(QuetzSharedData* shared) {
    const uintptr_t first = (uintptr_t(shared->local_ram_storage) +
        QUETZ_LOCAL_RAM_BYTES - 1) & ~uintptr_t(QUETZ_LOCAL_RAM_BYTES - 1);
    // Publish one relative offset; separate mmap views must never realign it.
    shared->local_ram_offset = uint32_t(first - uintptr_t(shared));
}

} // namespace Quetz
} // namespace SST

#endif // _SST_QUETZ_IPC_TYPES_H
