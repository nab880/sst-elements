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
 * Umbrella include: protocol types, modular tunnel collaborators, QuetzTunnel.
 * Compiled into both libquetz and libqemu_sst_plugin.
 */

#ifndef _SST_QUETZ_SHMEM_H
#define _SST_QUETZ_SHMEM_H

#include <stdlib.h>

#include <sst/core/interprocess/tunneldef.h>

#include "quetz_ipc_types.h"
#include "quetz_command_buffer.h"
#include "quetz_sync_manager.h"
#include "quetz_statistics_collector.h"

namespace SST {
namespace Quetz {

class QuetzTunnel
    : public SST::Core::Interprocess::TunnelDef<QuetzSharedData, QuetzCommand>
{
    using Base = SST::Core::Interprocess::TunnelDef<QuetzSharedData, QuetzCommand>;

public:
    QuetzTunnel(size_t numVCPUs, size_t bufferSize,
                uint32_t expectedChildren = 1)
        : Base(numVCPUs, bufferSize, expectedChildren),
          commands_(static_cast<Base*>(this)),
          sync_(),
          statistics_() {}

    explicit QuetzTunnel(void* shmPtr)
        : Base(shmPtr),
          commands_(static_cast<Base*>(this)),
          sync_(),
          statistics_() {}

    virtual uint32_t initialize(void* shmPtr) {
        uint32_t childnum = Base::initialize(shmPtr);
        sync_.bind(sharedData);
        statistics_.bindShared(sharedData);
        if (isMaster()) {
            sync_.initMaster(getNumBuffers());
            statistics_.initMaster();
        } else {
            sync_.announceAttach();
        }
        return childnum;
    }

    void waitForChild() { sync_.waitForChild(); }

    void writeMessage(size_t buffer, const QuetzCommand& command) {
        Base::writeMessage(buffer, command);
    }

    bool readMessageNB(size_t buffer, QuetzCommand* result) {
        return Base::readMessageNB(buffer, result);
    }

    void updateTime(uint64_t ns) { statistics_.updateSimTime(ns); }

    void incrementCycles() { statistics_.incrementSimCycles(); }

    uint64_t getCycles() const { return statistics_.getSimCycles(); }

    QuetzSharedData* getSharedData() { return sharedData; }

    QuetzCommandBuffer&       commands()       { return commands_; }
    const QuetzCommandBuffer& commands() const { return commands_; }
    QuetzSyncManager&               sync()           { return sync_; }
    QuetzStatisticsCollector&       statistics()     { return statistics_; }

private:
    QuetzCommandBuffer commands_;
    QuetzSyncManager         sync_;
    QuetzStatisticsCollector statistics_;
};

} // namespace Quetz
} // namespace SST

#endif // _SST_QUETZ_SHMEM_H
