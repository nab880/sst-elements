// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2026, NTESS
// All rights reserved.

/**
 * mem_access_handler.h — strategy for capturing guest memory accesses in
 * the tracing backend and writing them into the Quetz IPC tunnel.
 *
 * QemuMemAccessHandler implements the QEMU plugin callback path; a future
 * PIN backend would provide its own MemAccessHandler subclass.
 */

#ifndef _QUETZ_MEM_ACCESS_HANDLER_H
#define _QUETZ_MEM_ACCESS_HANDLER_H

#include "../quetz_shmem.h"

#include <glib.h>
extern "C" {
#include "qemu-plugin.h"
}

namespace SST {
namespace Quetz {

class MemAccessHandler {
public:
    virtual ~MemAccessHandler() = default;

    virtual void handle(unsigned int vcpu_index,
                        qemu_plugin_meminfo_t info,
                        uint64_t vaddr,
                        void* userdata,
                        QuetzInsnClass cls) = 0;
};

class QemuMemAccessHandler : public MemAccessHandler {
public:
    void handle(unsigned int vcpu_index,
                qemu_plugin_meminfo_t info,
                uint64_t vaddr,
                void* userdata,
                QuetzInsnClass cls) override;
};

MemAccessHandler* get_mem_access_handler();

} // namespace Quetz
} // namespace SST

#endif // _QUETZ_MEM_ACCESS_HANDLER_H
