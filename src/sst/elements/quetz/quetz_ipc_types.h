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
 * quetz_ipc_types.h — cross-process IPC wire types (no tunnel logic).
 *
 * Compiled into both libquetz and libqemu_sst_plugin.
 */

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
};

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

/** Per-vCPU slot for blocking MMIO round-trips (QEMU bridge / linux-user hook). */
struct QuetzMmioResponseSlot {
    volatile uint32_t ready;
    uint32_t          _pad;
    uint64_t          value;
};

/** Request mailbox written by QEMU; serviced by QuetzCPU each tick. */
struct QuetzMmioSyncRequest {
    volatile uint32_t pending;
    uint32_t          cmd;
    uint32_t          size;
    uint32_t          _pad;
    uint64_t          addr;
    uint64_t          write_val;
};

static constexpr unsigned QUETZ_MAX_MMIO_VCORES = 256;

struct QuetzSharedData {
    size_t            numCores;
    uint64_t          simTime;
    uint64_t          simCycles;
    volatile uint32_t child_attached;
    uint32_t          _pad0;
    QuetzMmioResponseSlot mmio_slot[QUETZ_MAX_MMIO_VCORES];
    QuetzMmioSyncRequest  mmio_req[QUETZ_MAX_MMIO_VCORES];
};

} // namespace Quetz
} // namespace SST

#endif // _SST_QUETZ_IPC_TYPES_H
