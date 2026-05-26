// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2026, NTESS
// All rights reserved.

#include "mem_access_handler.h"

#include "insn_classifier.h"
#include "plugin_state.h"
#include "registry.h"

#include <cstdint>
#include <cstring>

namespace SST {
namespace Quetz {

void QemuMemAccessHandler::handle(unsigned int vcpu_index,
                                  qemu_plugin_meminfo_t info,
                                  uint64_t vaddr,
                                  void* userdata,
                                  QuetzInsnClass cls) {
    if (vcpu_index >= PLUGIN_MAX_VCPUS)
        return;

    bool     is_store = qemu_plugin_mem_is_store(info);
    uint32_t shift    = qemu_plugin_mem_size_shift(info);
    uint32_t size     = (shift < 8) ? (1u << shift) : 128u;
    uint64_t pc       = (uint64_t)(uintptr_t)userdata;

    if (g_mmio_sync_size != 0 && vaddr >= g_mmio_sync_base &&
        vaddr < g_mmio_sync_base + g_mmio_sync_size) {
        return;
    }

    g_mem_seen[vcpu_index].store(true, std::memory_order_relaxed);

    if (g_insn_classifier && !g_insn_classifier->usesPreciseMemCallbacks())
        cls = g_insn_classifier->refineMemClass(cls, size);

    const uint8_t* store_data = nullptr;
    uint8_t        store_buf[sizeof(QuetzCommand::data)] = {0};
    if (is_store && size <= sizeof(QuetzCommand::data)) {
#if QEMU_PLUGIN_VERSION >= 4
        qemu_plugin_mem_value v = qemu_plugin_mem_get_value(info);
        switch (v.type) {
        case QEMU_PLUGIN_MEM_VALUE_U8:
            store_buf[0] = v.data.u8;
            break;
        case QEMU_PLUGIN_MEM_VALUE_U16:
            memcpy(store_buf, &v.data.u16, sizeof(v.data.u16));
            break;
        case QEMU_PLUGIN_MEM_VALUE_U32:
            memcpy(store_buf, &v.data.u32, sizeof(v.data.u32));
            break;
        case QEMU_PLUGIN_MEM_VALUE_U64:
            memcpy(store_buf, &v.data.u64, sizeof(v.data.u64));
            break;
        case QEMU_PLUGIN_MEM_VALUE_U128:
            memcpy(store_buf,     &v.data.u128.low,  sizeof(v.data.u128.low));
            memcpy(store_buf + 8, &v.data.u128.high, sizeof(v.data.u128.high));
            break;
        }
        store_data = store_buf;
#else
        if (!g_system_mode)
            store_data = reinterpret_cast<const uint8_t*>(
                static_cast<uintptr_t>(vaddr));
#endif
    }

    write_cmd(vcpu_index,
              is_store ? QUETZ_CMD_WRITE : QUETZ_CMD_READ,
              size, pc, vaddr, cls, store_data);
}

QUETZ_REGISTER_MEM_HANDLER("", QemuMemAccessHandler);

MemAccessHandler* get_mem_access_handler() {
    return g_mem_handler;
}

} // namespace Quetz
} // namespace SST
