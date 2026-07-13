/* Mirror of sst-elements/quetz/quetz_ipc_types.h for QEMU builds (C-compatible). */
#ifndef QUETZ_IPC_TYPES_H
#define QUETZ_IPC_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum QuetzShmemCmd {
    QUETZ_CMD_NOP            = 0,
    QUETZ_CMD_READ           = 1,
    QUETZ_CMD_WRITE          = 2,
    QUETZ_CMD_EXIT           = 3,
    QUETZ_CMD_MMIO_READ_REQ  = 4,
    QUETZ_CMD_MMIO_WRITE_REQ = 5,
};

typedef struct QuetzMmioResponseSlot {
    volatile uint32_t ready;
    uint32_t          _pad;
    uint64_t          value;
} QuetzMmioResponseSlot;

#define QUETZ_MAX_MMIO_VCORES 256

typedef struct QuetzMmioSyncRequest {
    volatile uint32_t pending;
    uint32_t          cmd;
    uint32_t          size;
    uint32_t          _pad;
    uint64_t          addr;
    uint64_t          write_val;
} QuetzMmioSyncRequest;

/* Reverse (SST -> guest) IRQ mailbox: one slot per (vcore, machine IRQ line).
 *
 * Single-writer seqlock, no handshake: SST (the only writer) stores `level`
 * and then release-stores an incremented `seq`; the QEMU bridge polls with an
 * acquire-load of `seq` and re-applies qemu_set_irq(level) whenever seq moved.
 * QEMU never writes the slot, so there is no lost-update window — a consumer
 * that pairs a stale seq with a newer level merely re-applies the same level
 * on its next poll tick.
 *
 * CONTRACT (level semantics, not edges): the slot carries the CURRENT line
 * level and the poller only observes the latest value — a raise followed by
 * a lower between two poll ticks collapses to the final level, so transient
 * pulses are lost BY DESIGN. SST devices must hold the line raised while
 * unconsumed work exists and lower it only once the guest has acked
 * everything; under that discipline the collapsed observation is correct.
 */
#define QUETZ_MAX_IRQ_LINES 64

typedef struct QuetzIrqSlot {
    volatile uint32_t seq;    /* release-store by SST, acquire-load by QEMU */
    uint32_t          level;  /* 1 = raise, 0 = lower */
} QuetzIrqSlot;

/* Layout stamp written by the SST master at init; verified before any use of
 * the region (quetz_ipc_attach). Last field on purpose: its offset moves if
 * this struct or SST-core's tunnel header drifts, so skew fails the attach
 * loudly instead of corrupting MMIO values silently. 'QZM' + layout version —
 * must match sst-elements/quetz/quetz_ipc_types.h exactly. */
#define QUETZ_SHM_MAGIC 0x515A4D02u

typedef struct QuetzSharedData {
    size_t            numCores;
    uint64_t          simTime;
    uint64_t          simCycles;
    volatile uint32_t child_attached;
    uint32_t          _pad0;
    QuetzMmioResponseSlot mmio_slot[QUETZ_MAX_MMIO_VCORES];
    QuetzMmioSyncRequest  mmio_req[QUETZ_MAX_MMIO_VCORES];
    QuetzIrqSlot          irq_slot[QUETZ_MAX_MMIO_VCORES][QUETZ_MAX_IRQ_LINES];
    /* Bumped (release) by SST on every postIrq; the bridge's poll tick
     * acquire-loads it and skips the whole irq_slot scan when unchanged. */
    volatile uint32_t irq_generation;
    uint32_t          _pad2;
    volatile uint32_t magic;   /* QUETZ_SHM_MAGIC — keep as the LAST field */
    uint32_t          _pad1;
} QuetzSharedData;

#ifdef __cplusplus
}
#endif

#endif
