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
 * quetz_shmem.h — shared-memory IPC tunnel between the QuetzComponent (parent)
 * and the QEMU TCG plugin (child).
 *
 * This file is compiled into BOTH sides:
 *   - The SST component (libquetz.so) — full C++17 / SST include path
 *   - The QEMU plugin (libqemu_sst_plugin.so) — compiled with SST headers
 *     on the include path, but no SST link-time dependency (everything here
 *     is header-only templates).
 *
 * Design mirrors ariel/ariel_shmem.h.
 */

#ifndef _SST_QUETZ_SHMEM_H
#define _SST_QUETZ_SHMEM_H

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>   // exit() — needed before tunneldef.h template instantiation
#include <sys/time.h>

// TunnelDef and CircularBuffer are header-only templates — no SST link dep.
#include <sst/core/interprocess/tunneldef.h>

namespace SST {
namespace Quetz {

// ---------------------------------------------------------------------------
// Commands written by the QEMU plugin and read by the SST component.
// ---------------------------------------------------------------------------
enum QuetzShmemCmd : uint32_t {
    QUETZ_CMD_NOP   = 0,   //< instruction with no memory side-effect
    QUETZ_CMD_READ  = 1,   //< memory load
    QUETZ_CMD_WRITE = 2,   //< memory store
    QUETZ_CMD_EXIT  = 3,   //< guest application exiting
};

/**
 * Instruction class — used by SST to apply per-class execution latencies
 * and per-class statistics.  Populated by the plugin at TB-translation time
 * from the instruction encoding.
 *
 * Memory classes (0-2): set on READ/WRITE commands; exec_latency applies.
 * Compute classes (3-6): set on NOP commands when detailed_instruction_tracking
 *   is enabled; exec_latency is unused for these.
 */
enum QuetzInsnClass : uint32_t {
    QUETZ_INSN_INT_MEM      = 0,  //< integer load / store
    QUETZ_INSN_FP_MEM       = 1,  //< scalar floating-point load / store
    QUETZ_INSN_VEC_MEM      = 2,  //< vector load / store (any element width)
    QUETZ_INSN_INT_COMPUTE  = 3,  //< integer ALU (non-memory)
    QUETZ_INSN_FP_COMPUTE   = 4,  //< floating-point arithmetic (non-memory)
    QUETZ_INSN_VEC_COMPUTE  = 5,  //< vector / SIMD arithmetic (non-memory)
    QUETZ_INSN_BRANCH       = 6,  //< branch, jump, call, return
    QUETZ_INSN_OTHER        = 7,  //< SYSTEM, CSR, unclassified
    QUETZ_INSN_CLASS_COUNT  = 8
};

/**
 * One entry in the per-vCPU circular buffer.
 *
 * For NOP:        size==0, addr==0, pc==instruction PC
 * For READ/WRITE: size = access width (bytes), addr = guest virtual address,
 *                 pc = instruction PC, insn_class = QuetzInsnClass value
 * For EXIT:       all fields 0
 */
struct QuetzCommand {
    QuetzShmemCmd cmd;
    uint32_t      size;        // memory access width in bytes
    uint64_t      pc;          // guest instruction pointer
    uint64_t      addr;        // guest virtual address (READ/WRITE only)
    uint32_t      insn_class;  // QuetzInsnClass — instruction category
    uint32_t      _pad;        // reserved / alignment padding
    // For WRITE commands: actual store data (up to 16 bytes, little-endian).
    // Bytes beyond min(size, 16) are undefined.  For READ/NOP/EXIT: unused.
    uint8_t       data[16];
};

// ---------------------------------------------------------------------------
// Shared region — lives at the beginning of the POSIX shared memory segment.
// ---------------------------------------------------------------------------
struct QuetzSharedData {
    size_t   numCores;          // number of vCPU buffers
    uint64_t simTime;           // updated by SST each cycle (nanoseconds)
    uint64_t simCycles;         // SST cycle count
    volatile uint32_t child_attached;  // incremented by plugin on attach
    uint32_t _pad0;
};

// ---------------------------------------------------------------------------
// Tunnel type — one circular buffer per vCPU.
// ---------------------------------------------------------------------------
class QuetzTunnel
    : public SST::Core::Interprocess::TunnelDef<QuetzSharedData, QuetzCommand>
{
    using Base = SST::Core::Interprocess::TunnelDef<QuetzSharedData, QuetzCommand>;

public:
    /** Construct the master-side tunnel (SST component) */
    QuetzTunnel(size_t numVCPUs, size_t bufferSize,
                uint32_t expectedChildren = 1)
        : Base(numVCPUs, bufferSize, expectedChildren) {}

    /** Attach to an existing tunnel (QEMU plugin side) */
    explicit QuetzTunnel(void* shmPtr) : Base(shmPtr) {}

    /** Called by both sides after the shared memory is mapped */
    virtual uint32_t initialize(void* shmPtr) {
        uint32_t childnum = Base::initialize(shmPtr);
        if (isMaster()) {
            sharedData->numCores       = getNumBuffers();
            sharedData->simTime        = 0;
            sharedData->simCycles      = 0;
            sharedData->child_attached = 0;
        } else {
            // Plugin side — announce we are attached
            __sync_fetch_and_add(&sharedData->child_attached, 1u);
        }
        return childnum;
    }

    /** SST component spins here until the plugin has attached */
    void waitForChild() {
        while (sharedData->child_attached == 0)
            __sync_synchronize();
    }

    void updateTime(uint64_t ns)   { sharedData->simTime   = ns; }
    void incrementCycles()         { sharedData->simCycles++;    }
    uint64_t getCycles()     const { return sharedData->simCycles; }

    /** Access the raw shared data region (for plugin-side use). */
    QuetzSharedData* getSharedData() { return sharedData; }
};

} // namespace Quetz
} // namespace SST

#endif // _SST_QUETZ_SHMEM_H
