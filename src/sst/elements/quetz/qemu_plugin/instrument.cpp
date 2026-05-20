// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "instrument.h"

#include "decoder_aarch64.h"
#include "decoder_generic.h"
#include "decoder_riscv.h"
#include "plugin_state.h"

#include <cstdint>
#include <cstring>

using namespace SST::Quetz;

static inline void handle_mem(unsigned int vcpu_index,
                               qemu_plugin_meminfo_t info,
                               uint64_t vaddr,
                               void* userdata,
                               QuetzInsnClass cls)
{
    if (vcpu_index >= PLUGIN_MAX_VCPUS) return;

    bool     is_store = qemu_plugin_mem_is_store(info);
    uint32_t shift    = qemu_plugin_mem_size_shift(info);
    uint32_t size     = (shift < 8) ? (1u << shift) : 128u;
    uint64_t pc       = (uint64_t)(uintptr_t)userdata;

    g_mem_seen[vcpu_index].store(true, std::memory_order_relaxed);

    if (g_isa == QUETZ_ISA_GENERIC)
        cls = classify_by_size(size);

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

static void cb_mem_int(unsigned int vi, qemu_plugin_meminfo_t info,
                       uint64_t va, void* ud)
{ handle_mem(vi, info, va, ud, QUETZ_INSN_INT_MEM); }

static void cb_mem_fp(unsigned int vi, qemu_plugin_meminfo_t info,
                      uint64_t va, void* ud)
{ handle_mem(vi, info, va, ud, QUETZ_INSN_FP_MEM); }

static void cb_mem_vec(unsigned int vi, qemu_plugin_meminfo_t info,
                       uint64_t va, void* ud)
{ handle_mem(vi, info, va, ud, QUETZ_INSN_VEC_MEM); }

static inline void emit_nop_now(unsigned int vcpu_index, void* userdata,
                                 QuetzInsnClass cls)
{
    if (vcpu_index >= PLUGIN_MAX_VCPUS) return;
    uint64_t pc = (uint64_t)(uintptr_t)userdata;
    write_cmd(vcpu_index, QUETZ_CMD_NOP, 0, pc, 0, cls);
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
        write_cmd(vcpu_index, QUETZ_CMD_NOP, 0, pc, 0, prev_cls);
    }
}

static void cb_exec_delayed_other(unsigned int vi, void* ud)
{ handle_exec_delayed(vi, ud, QUETZ_INSN_OTHER); }

static void cb_tb_trans(qemu_plugin_id_t /*id*/, struct qemu_plugin_tb* tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn* insn = qemu_plugin_tb_get_insn(tb, i);
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

        if (g_isa == QUETZ_ISA_GENERIC) {
            qemu_plugin_register_vcpu_mem_cb(insn, cb_mem_int,
                                              QEMU_PLUGIN_CB_NO_REGS,
                                              QEMU_PLUGIN_MEM_RW, pc_ptr);
            qemu_plugin_register_vcpu_insn_exec_cb(insn, cb_exec_delayed_other,
                                                    QEMU_PLUGIN_CB_NO_REGS,
                                                    pc_ptr);
            continue;
        }

        QuetzInsnClass cls = (g_isa == QUETZ_ISA_RISCV)
            ? classify_riscv_insn(enc)
            : classify_aarch64_insn(enc);

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

static void cb_vcpu_init(qemu_plugin_id_t /*id*/, unsigned int vcpu_index)
{
    if (vcpu_index < PLUGIN_MAX_VCPUS) {
        g_mem_seen[vcpu_index].store(false, std::memory_order_relaxed);
        g_prev_cls[vcpu_index] = QUETZ_INSN_OTHER;
    }
}

void register_plugin_callbacks(qemu_plugin_id_t id)
{
    qemu_plugin_register_vcpu_init_cb(id, cb_vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, cb_tb_trans);
}
