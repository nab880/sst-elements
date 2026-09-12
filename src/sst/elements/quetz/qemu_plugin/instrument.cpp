// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "instrument.h"
#include "coldfire_cache_ops.h"

#include "insn_classifier.h"
#include "mem_access_handler.h"
#include "plugin_state.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#endif

using namespace SST::Quetz;

#if QEMU_PLUGIN_VERSION >= 3
// Register handles belong to their vCPU and are discovered only after init.
static qemu_plugin_register* cache_registers[PLUGIN_MAX_VCPUS][16]{};
static qemu_plugin_register* cache_status[PLUGIN_MAX_VCPUS]{};
static GByteArray* cache_register_bytes[PLUGIN_MAX_VCPUS]{};

[[noreturn]] static void cache_op_fatal(const char* reason)
{
    fprintf(stderr, "[qemu_sst_plugin] ERROR: cache_ops=1: %s\n", reason);
    exit(EXIT_FAILURE);
}

static uint32_t read_cache_register(unsigned vi, qemu_plugin_register* reg)
{
    GByteArray* bytes = cache_register_bytes[vi];
    g_byte_array_set_size(bytes, 0);
    if (qemu_plugin_read_register(reg, bytes) != 4 || bytes->len != 4)
        cache_op_fatal("could not read a ColdFire 32-bit register");
    return (uint32_t(bytes->data[0]) << 24) |
           (uint32_t(bytes->data[1]) << 16) |
           (uint32_t(bytes->data[2]) << 8) | bytes->data[3];
}

static void init_cache_registers(unsigned vi)
{
    if (!g_cache_ops) return;
    if (vi >= PLUGIN_MAX_VCPUS)
        cache_op_fatal("vCPU index exceeds mailbox capacity");
    GArray* regs = qemu_plugin_get_registers();
    if (!regs) cache_op_fatal("ColdFire register descriptors unavailable");
    bool coldfire = false;
    for (guint i = 0; i < regs->len; ++i) {
        const auto& reg = g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (reg.feature && strcmp(reg.feature, "org.gnu.gdb.coldfire.core") == 0)
            coldfire = true;
        if (!reg.name) continue;
        if (reg.name[0] == 'd' && reg.name[1] >= '0' && reg.name[1] <= '7' &&
            reg.name[2] == '\0')
            cache_registers[vi][reg.name[1] - '0'] = reg.handle;
        else if (reg.name[0] == 'a' && reg.name[1] >= '0' && reg.name[1] <= '7' &&
                 reg.name[2] == '\0')
            cache_registers[vi][8 + reg.name[1] - '0'] = reg.handle;
        else if (strcmp(reg.name, "fp") == 0) cache_registers[vi][14] = reg.handle;
        else if (strcmp(reg.name, "sp") == 0) cache_registers[vi][15] = reg.handle;
        else if (strcmp(reg.name, "ps") == 0 || strcmp(reg.name, "sr") == 0)
            cache_status[vi] = reg.handle;
    }
    g_array_free(regs, true);
    if (!coldfire || !cache_status[vi])
        cache_op_fatal("ColdFire register descriptors (including SR) unavailable");
    for (const auto* reg : cache_registers[vi])
        if (!reg) cache_op_fatal("ColdFire D/A register descriptor unavailable");
    cache_register_bytes[vi] = g_byte_array_sized_new(4);
}

static void cb_cache_op(unsigned vi, void* userdata)
{
    const uint32_t encoding = uint32_t(uintptr_t(userdata));
    const uint8_t bytes[] = { uint8_t(encoding >> 24), uint8_t(encoding >> 16),
                             uint8_t(encoding >> 8), uint8_t(encoding) };
    const auto op = decodeColdFireCacheOp(bytes, sizeof(bytes));
    if (vi >= PLUGIN_MAX_VCPUS || !op.valid)
        cache_op_fatal("invalid cache-op callback");
    // Callbacks run before the instruction. Do not model an operation that
    // QEMU would reject with a privilege exception. This profile is supervisor
    // firmware only; unsupported use fails instead of hiding that exception.
    if ((read_cache_register(vi, cache_status[vi]) & 0x2000) == 0)
        cache_op_fatal("cache instructions require supervisor firmware");
    const uint32_t value = op.kind == 0
        ? read_cache_register(vi, cache_registers[vi][op.source])
        : op.instruction;
    flush_run(vi);
    auto* shared = g_tunnel->getSharedData();
    if (!shared || shared->magic != QUETZ_SHM_MAGIC)
        cache_op_fatal("invalid shared-memory cache mailbox");
    auto* req = &shared->mmio_req[vi];
    auto* slot = &shared->mmio_slot[vi];
    if (__atomic_load_n(&req->pending, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE) != 0)
        cache_op_fatal("synchronous cache mailbox already in use");
    req->addr = op.kind == 0 ? op.control
        : read_cache_register(vi, cache_registers[vi][op.source]);
    req->size = op.kind;
    req->write_val = value;
    req->cmd = QUETZ_CMD_CACHE_OP;
    __atomic_store_n(&req->pending, 1u, __ATOMIC_RELEASE);
    // The same vCPU owns the bridge mailbox. Waiting for acknowledgment keeps
    // maintenance ordered with window accesses and device doorbells. It makes
    // no ordering claim about unrelated ordinary-RAM trace traffic.
    while (__atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE) == 0) {
#ifdef __linux__
        struct timespec timeout = {0, 1000000};
        syscall(SYS_futex, &slot->ready, FUTEX_WAIT, 0, &timeout, nullptr, 0);
#else
        g_usleep(1000);
#endif
    }
    __atomic_store_n(&slot->ready, 0u, __ATOMIC_RELEASE);
}

static void instrument_cache_op(qemu_plugin_insn* insn)
{
    if (!g_cache_ops) return;
    uint8_t bytes[4]{};
    const size_t size = qemu_plugin_insn_data(insn, bytes, sizeof(bytes));
    if (!decodeColdFireCacheOp(bytes, size).valid) return;
    const uint32_t encoding = (uint32_t(bytes[0]) << 24) |
        (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
    qemu_plugin_register_vcpu_insn_exec_cb(insn, cb_cache_op,
        QEMU_PLUGIN_CB_R_REGS, reinterpret_cast<void*>(uintptr_t(encoding)));
}
#else
static void init_cache_registers(unsigned) {}
static void instrument_cache_op(qemu_plugin_insn*) {}
#endif

static void cb_mem_int(unsigned int vi, qemu_plugin_meminfo_t info,
                       uint64_t va, void* ud)
{
    get_mem_access_handler()->handle(vi, info, va, ud, QUETZ_INSN_INT_MEM);
}

static void cb_mem_fp(unsigned int vi, qemu_plugin_meminfo_t info,
                      uint64_t va, void* ud)
{
    get_mem_access_handler()->handle(vi, info, va, ud, QUETZ_INSN_FP_MEM);
}

static void cb_mem_vec(unsigned int vi, qemu_plugin_meminfo_t info,
                       uint64_t va, void* ud)
{
    get_mem_access_handler()->handle(vi, info, va, ud, QUETZ_INSN_VEC_MEM);
}

static inline void emit_nop_now(unsigned int vcpu_index, void* userdata,
                                 QuetzInsnClass cls)
{
    uint64_t pc = (uint64_t)(uintptr_t)userdata;
    accumulate_compute(vcpu_index, pc, cls);
}

static void cb_emit_nop_icomp(unsigned int vi, void* ud)
{ emit_nop_now(vi, ud, QUETZ_INSN_INT_COMPUTE); }
static void cb_emit_nop_fcomp(unsigned int vi, void* ud)
{ emit_nop_now(vi, ud, QUETZ_INSN_FP_COMPUTE); }
static void cb_emit_nop_vcomp(unsigned int vi, void* ud)
{ emit_nop_now(vi, ud, QUETZ_INSN_VEC_COMPUTE); }
static void cb_emit_nop_branch(unsigned int vi, void* ud)
{ emit_nop_now(vi, ud, QUETZ_INSN_BRANCH); }
static void cb_emit_nop_other(unsigned int vi, void* ud)
{ emit_nop_now(vi, ud, QUETZ_INSN_OTHER); }

static qemu_plugin_vcpu_udata_cb_t const g_emit_nop_cbs[QUETZ_INSN_CLASS_COUNT] = {
    nullptr,
    nullptr,
    nullptr,
    cb_emit_nop_icomp,
    cb_emit_nop_fcomp,
    cb_emit_nop_vcomp,
    cb_emit_nop_branch,
    cb_emit_nop_other,
};

static inline void handle_exec_delayed(unsigned int vcpu_index, void* userdata,
                                        QuetzInsnClass cls)
{
    if (vcpu_index >= PLUGIN_MAX_VCPUS) return;

    QuetzInsnClass prev_cls = g_prev_cls[vcpu_index];
    g_prev_cls[vcpu_index]  = cls;

    bool had_mem = g_mem_seen[vcpu_index].exchange(false,
                                                    std::memory_order_relaxed);
    if (!had_mem) {
        uint64_t pc = (uint64_t)(uintptr_t)userdata;
        accumulate_compute(vcpu_index, pc, prev_cls);
    }
}

static void cb_exec_delayed_other(unsigned int vi, void* ud)
{ handle_exec_delayed(vi, ud, QUETZ_INSN_OTHER); }

static void cb_tb_trans(qemu_plugin_id_t , struct qemu_plugin_tb* tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn* insn = qemu_plugin_tb_get_insn(tb, i);
        instrument_cache_op(insn);
        uint64_t pc     = qemu_plugin_insn_vaddr(insn);
        void*    pc_ptr = (void*)(uintptr_t)pc;

        uint32_t enc = 0;
        {
#if QEMU_PLUGIN_VERSION >= 3
            qemu_plugin_insn_data(insn, &enc, sizeof(enc));
#else
            const void* raw  = qemu_plugin_insn_data(insn);
            size_t      isz  = qemu_plugin_insn_size(insn);
            size_t      copy = isz < sizeof(enc) ? isz : sizeof(enc);
            if (raw) memcpy(&enc, raw, copy);
#endif
        }

        if (!g_insn_classifier->usesPreciseMemCallbacks()) {
            qemu_plugin_register_vcpu_mem_cb(insn, cb_mem_int,
                                              QEMU_PLUGIN_CB_NO_REGS,
                                              QEMU_PLUGIN_MEM_RW, pc_ptr);
            qemu_plugin_register_vcpu_insn_exec_cb(insn, cb_exec_delayed_other,
                                                    QEMU_PLUGIN_CB_NO_REGS,
                                                    pc_ptr);
            continue;
        }

        QuetzInsnClass cls = g_insn_classifier->classify(enc);

        bool is_mem = (cls == QUETZ_INSN_INT_MEM ||
                       cls == QUETZ_INSN_FP_MEM  ||
                       cls == QUETZ_INSN_VEC_MEM);

        if (is_mem) {
            qemu_plugin_vcpu_mem_cb_t mem_cb =
                  (cls == QUETZ_INSN_FP_MEM)  ? cb_mem_fp
                : (cls == QUETZ_INSN_VEC_MEM) ? cb_mem_vec
                :                                cb_mem_int;
            qemu_plugin_register_vcpu_mem_cb(insn, mem_cb,
                                              QEMU_PLUGIN_CB_NO_REGS,
                                              QEMU_PLUGIN_MEM_RW, pc_ptr);
        } else {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, g_emit_nop_cbs[cls],
                                                    QEMU_PLUGIN_CB_NO_REGS,
                                                    pc_ptr);
        }
    }
}

static void cb_vcpu_init(qemu_plugin_id_t , unsigned int vcpu_index)
{
    require_vcpu(vcpu_index);
    init_cache_registers(vcpu_index);
    if (vcpu_index < PLUGIN_MAX_VCPUS) {
        g_mem_seen[vcpu_index].store(false, std::memory_order_relaxed);
        g_prev_cls[vcpu_index] = QUETZ_INSN_OTHER;
        g_run_count[vcpu_index] = 0;
    }
}

void register_plugin_callbacks(qemu_plugin_id_t id)
{
    qemu_plugin_register_vcpu_init_cb(id, cb_vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, cb_tb_trans);
}
