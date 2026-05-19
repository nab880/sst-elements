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
 * qemu_sst_plugin.cpp — QEMU TCG plugin that bridges a guest binary to SST.
 *
 * The plugin is loaded by QEMU via:
 *   qemu-riscv64 -plugin libqemu_sst_plugin.so,shmname=<region>[,detailed=1] <exe>
 *
 * For each vCPU (QEMU thread) the plugin:
 *   1. Opens the POSIX shared-memory region created by QuetzComponent.
 *   2. For every translation block (TB) instruments:
 *        - A per-instruction memory callback for loads/stores → READ/WRITE
 *        - A per-instruction exec callback for non-memory ops → NOP
 *   3. On application exit sends QUETZ_CMD_EXIT on every vCPU buffer.
 *
 * When detailed=1 is passed, each NOP carries the actual instruction class
 * (INT_COMPUTE, FP_COMPUTE, VEC_COMPUTE, BRANCH, or OTHER) instead of OTHER.
 * ISAs without a full decoder emit a warning and leave all non-memory
 * instructions classified as OTHER.
 */

// qemu-plugin.h is a C header without its own extern-C guards.
// Wrap it so all its declarations get C linkage when compiled as C++.
extern "C" {
#include "qemu-plugin.h"
}

#include "../quetz_shmem.h"
#include <sst/core/interprocess/shmchild.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <string>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

using namespace SST::Quetz;
using SHMChild = SST::Core::Interprocess::SHMChild<QuetzTunnel>;

// ---------------------------------------------------------------------------
// Global plugin state
// ---------------------------------------------------------------------------
static SHMChild*     g_shmchild = nullptr;
static QuetzTunnel*  g_tunnel   = nullptr;
static std::string   g_shmem_name;
static bool          g_detailed     = false;   // detailed instruction tracking
static bool          g_system_mode  = false;   // true for qemu-system-* targets

static constexpr unsigned MAX_VCPUS = 256;
static std::atomic<bool> g_mem_seen[MAX_VCPUS];

// Per-vCPU class of the most recently *started* instruction.
// The exec callback fires at the start of instruction N and sends a NOP for
// instruction N-1 (if N-1 had no memory access).  To tag that NOP with N-1's
// class we need to remember what N-1 was; that is stored here by exec_cb(N-1)
// and consumed by exec_cb(N).  Single-writer per slot (the guest vCPU thread),
// so no atomics required.
static QuetzInsnClass g_prev_cls[MAX_VCPUS];

// ISA detected from qemu_info_t::target_name at plugin install time.
enum QuetzISA { QUETZ_ISA_RISCV, QUETZ_ISA_AARCH64, QUETZ_ISA_GENERIC };
static QuetzISA g_isa = QUETZ_ISA_GENERIC;

// ---------------------------------------------------------------------------
// How to add a decoder for a new ISA
// ---------------------------------------------------------------------------
//
// 1. Add a new enumerator to QuetzISA above (e.g. QUETZ_ISA_MIPS).
//
// 2. Implement a classifier:
//
//      static inline QuetzInsnClass classify_mips_insn(uint32_t enc) {
//          // Map MIPS opcode/funct fields to QuetzInsnClass values:
//          //   QUETZ_INSN_INT_MEM / FP_MEM / VEC_MEM  — loads and stores
//          //   QUETZ_INSN_INT_COMPUTE / FP_COMPUTE / VEC_COMPUTE — ALU
//          //   QUETZ_INSN_BRANCH  — branches, jumps, calls, returns
//          //   QUETZ_INSN_OTHER   — traps, coprocessor ops, unclassified
//          uint32_t op = (enc >> 26) & 0x3F;
//          ...
//      }
//
//    For variable-length ISAs (x86): qemu_plugin_insn_size() returns the
//    true instruction length; qemu_plugin_insn_data() only fills up to
//    sizeof(uint32_t) bytes, so use the size to decide how much is valid.
//
// 3. In qemu_plugin_install(), extend the target-name check:
//      else if (strncmp(t, "mips", 4) == 0) g_isa = QUETZ_ISA_MIPS;
//
// 4. In cb_tb_trans(), add a case to the switch:
//      case QUETZ_ISA_MIPS: cls = classify_mips_insn(enc); break;

// ---------------------------------------------------------------------------
// RISC-V instruction classifier
//
// Covers the full instruction set including compute and branch instructions.
// 16-bit RVC instructions (bits[1:0] != 11) are conservatively tagged
// INT_COMPUTE; sub-classifying them is possible but complex.
// AMO (opcode 0x2F) is treated as INT_MEM (atomic load-modify-store).
// SYSTEM/CSR (opcode 0x73) is tagged OTHER.
// ---------------------------------------------------------------------------
static inline QuetzInsnClass classify_riscv_insn(const uint32_t enc)
{
    if ((enc & 0x3u) != 0x3u)
        return QUETZ_INSN_INT_COMPUTE;  // 16-bit RVC (conservative)

    const uint32_t opcode = enc & 0x7Fu;
    const uint32_t funct3 = (enc >> 12) & 0x7u;

    switch (opcode) {
    // ---- Integer load / store ------------------------------------------
    case 0x03u: case 0x23u:
        return QUETZ_INSN_INT_MEM;
    // ---- FP / vector load / store (LOAD-FP, STORE-FP) -----------------
    case 0x07u: case 0x27u:
        if (funct3 == 0x2u || funct3 == 0x3u || funct3 == 0x4u)
            return QUETZ_INSN_FP_MEM;
        return QUETZ_INSN_VEC_MEM;
    // ---- Atomic memory operations (AMO) --------------------------------
    case 0x2Fu:
        return QUETZ_INSN_INT_MEM;
    // ---- FP fused multiply-add -----------------------------------------
    case 0x43u: case 0x47u: case 0x4Bu: case 0x4Fu:
        return QUETZ_INSN_FP_COMPUTE;
    // ---- FP arithmetic (OP-FP) -----------------------------------------
    case 0x53u:
        return QUETZ_INSN_FP_COMPUTE;
    // ---- Vector arithmetic (OP-V) --------------------------------------
    case 0x57u:
        return QUETZ_INSN_VEC_COMPUTE;
    // ---- Branches / jumps / calls / returns ----------------------------
    case 0x63u:                         // BRANCH (beq, bne, blt, bge, …)
    case 0x67u:                         // JALR
    case 0x6Fu:                         // JAL
        return QUETZ_INSN_BRANCH;
    // ---- Integer compute -----------------------------------------------
    case 0x13u: case 0x33u:             // OP-IMM / OP
    case 0x1Bu: case 0x3Bu:             // OP-IMM-32 / OP-32 (RV64)
    case 0x17u: case 0x37u:             // AUIPC / LUI
    case 0x0Fu:                         // MISC-MEM (fence)
        return QUETZ_INSN_INT_COMPUTE;
    // ---- System / CSR --------------------------------------------------
    case 0x73u:
        return QUETZ_INSN_OTHER;
    default:
        return QUETZ_INSN_OTHER;
    }
}

// ---------------------------------------------------------------------------
// AArch64 instruction classifier
//
// Uses bits[28:25] as the top-level encoding group (ARM ARM, section C3).
// Load/Store instructions are identified by bit[27]=1 AND bit[25]=0; within
// that group bit[26] distinguishes integer registers (=0) from SIMD/FP (=1).
//
// Group 0b1010/1011 covers branches alongside exception-generating
// instructions (SVC, BRK) and hints (NOP, ISB).  These are all tagged BRANCH
// as an approximation; for most programs branches dominate this group.
// ---------------------------------------------------------------------------
static inline QuetzInsnClass classify_aarch64_insn(const uint32_t enc)
{
    // Load/Store: bit[27]=1 AND bit[25]=0.
    // Within this group bit[26]=0 → integer register; bit[26]=1 → SIMD/FP.
    if ((enc & (1u << 27u)) && !(enc & (1u << 25u)))
        return (enc & (1u << 26u)) ? QUETZ_INSN_VEC_MEM : QUETZ_INSN_INT_MEM;

    // bits[28:25] — top-level encoding group
    const uint32_t grp = (enc >> 25u) & 0xFu;

    switch (grp) {
    // Data Processing – Immediate (ADD/SUB/AND/ORR/MOV immediate, ADRP …)
    case 0x8u: case 0x9u:
        return QUETZ_INSN_INT_COMPUTE;

    // Data Processing – Register (integer, ADD/SUB/AND/ORR register, shifts …)
    case 0x5u: case 0xDu:
        return QUETZ_INSN_INT_COMPUTE;

    // Advanced SIMD / NEON (FADD Vn, FMUL Vn, TBL, EXT, …)
    case 0x7u:
        return QUETZ_INSN_VEC_COMPUTE;

    // Scalar FP and FMADD/FMSUB/FNMADD/FNMSUB
    case 0xEu: case 0xFu:
        return QUETZ_INSN_FP_COMPUTE;

    // Branches, Exception Generating, System (B, BL, B.cond, BR, BLR,
    // RET, CBZ/CBNZ, TBZ/TBNZ, SVC, BRK, NOP, ISB, DMB, DSB)
    case 0x2u: case 0x3u: case 0xAu: case 0xBu:
        return QUETZ_INSN_BRANCH;

    // SVE instructions (AArch64 v8.2+, group 0b0001) — treat as vector
    case 0x1u:
        return QUETZ_INSN_VEC_COMPUTE;

    // Reserved, unallocated
    case 0x0u:
    default:
        return QUETZ_INSN_OTHER;
    }
}

// ---------------------------------------------------------------------------
// Generic size-based fallback for ISAs without a full decoder.
// Width >= 16 bytes implies a wide SIMD move; narrower accesses are integer.
// ---------------------------------------------------------------------------
static inline QuetzInsnClass classify_by_size(const uint32_t size)
{
    return (size >= 16) ? QUETZ_INSN_VEC_MEM : QUETZ_INSN_INT_MEM;
}

// ---------------------------------------------------------------------------
static inline void write_cmd(unsigned vcpu, QuetzShmemCmd type,
                              uint32_t size, uint64_t pc, uint64_t addr,
                              QuetzInsnClass cls = QUETZ_INSN_OTHER,
                              const uint8_t* store_data = nullptr)
{
    QuetzCommand cmd;
    cmd.cmd        = type;
    cmd.size       = size;
    cmd.pc         = pc;
    cmd.addr       = addr;
    cmd.insn_class = static_cast<uint32_t>(cls);
    cmd._pad       = 0;
    if (store_data) {
        uint32_t n = (size < sizeof(cmd.data)) ? size : (uint32_t)sizeof(cmd.data);
        memcpy(cmd.data, store_data, n);
        if (n < sizeof(cmd.data))
            memset(cmd.data + n, 0, sizeof(cmd.data) - n);
    } else {
        memset(cmd.data, 0, sizeof(cmd.data));
    }
    g_tunnel->writeMessage((size_t)vcpu, cmd);
}

// ---------------------------------------------------------------------------
// Memory-access callback
// ---------------------------------------------------------------------------
static inline void handle_mem(unsigned int vcpu_index,
                               qemu_plugin_meminfo_t info,
                               uint64_t vaddr,
                               void* userdata,
                               QuetzInsnClass cls)
{
    if (vcpu_index >= MAX_VCPUS) return;

    bool     is_store = qemu_plugin_mem_is_store(info);
    uint32_t shift    = qemu_plugin_mem_size_shift(info);
    uint32_t size     = (shift < 8) ? (1u << shift) : 128u;
    uint64_t pc       = (uint64_t)(uintptr_t)userdata;

    g_mem_seen[vcpu_index].store(true, std::memory_order_relaxed);

    // For ISAs where TB-translation couldn't determine the instruction class,
    // fall back to a size-based heuristic.
    if (g_isa == QUETZ_ISA_GENERIC)
        cls = classify_by_size(size);

    // Store-data capture.
    //
    // Preferred path (QEMU plugin API >= 4, QEMU 9.0+): qemu_plugin_mem_get_value
    // returns the actual value from the simulated CPU's register file as a
    // tagged union (U8/U16/U32/U64/U128).  Works in both user-mode and
    // system-mode QEMU and is the only API-blessed way to read store data.
    //
    // Fallback (API v1-v3): cast the guest VA to a host pointer.  This works
    // only in user-mode where QEMU maps the guest 1:1 into the host VA space;
    // in system-mode the guest VA is unrelated to host VAs and the read would
    // segfault, so the fallback is gated on !g_system_mode.
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

// ---------------------------------------------------------------------------
// Instruction exec callbacks — one per instruction class.
//
// The "one-instruction-delayed" pattern: when instruction N's exec callback
// fires, g_mem_seen reflects whether instruction N-1 issued a memory access.
// If not, we send a NOP carrying instruction N-1's class.  The class is
// baked into the callback function pointer at TB-translation time.
// ---------------------------------------------------------------------------
static inline void handle_exec(unsigned int vcpu_index, void* userdata,
                                QuetzInsnClass cls)
{
    if (vcpu_index >= MAX_VCPUS) return;

    // Carry the *previous* instruction's class into the NOP we're about to
    // send for it, then record the current instruction's class for next time.
    QuetzInsnClass prev_cls = g_prev_cls[vcpu_index];
    g_prev_cls[vcpu_index]  = cls;

    bool had_mem = g_mem_seen[vcpu_index].exchange(false,
                                                    std::memory_order_relaxed);
    if (!had_mem) {
        uint64_t pc = (uint64_t)(uintptr_t)userdata;
        write_cmd(vcpu_index, QUETZ_CMD_NOP, 0, pc, 0, prev_cls);
    }
}

// One callback function per class — class is implicit in the function pointer.
static void cb_exec_int   (unsigned int vi, void* ud)
{ handle_exec(vi, ud, QUETZ_INSN_INT_MEM);     }
static void cb_exec_fp    (unsigned int vi, void* ud)
{ handle_exec(vi, ud, QUETZ_INSN_FP_MEM);      }
static void cb_exec_vec   (unsigned int vi, void* ud)
{ handle_exec(vi, ud, QUETZ_INSN_VEC_MEM);     }
static void cb_exec_icomp (unsigned int vi, void* ud)
{ handle_exec(vi, ud, QUETZ_INSN_INT_COMPUTE); }
static void cb_exec_fcomp (unsigned int vi, void* ud)
{ handle_exec(vi, ud, QUETZ_INSN_FP_COMPUTE);  }
static void cb_exec_vcomp (unsigned int vi, void* ud)
{ handle_exec(vi, ud, QUETZ_INSN_VEC_COMPUTE); }
static void cb_exec_branch(unsigned int vi, void* ud)
{ handle_exec(vi, ud, QUETZ_INSN_BRANCH);      }
static void cb_exec_other (unsigned int vi, void* ud)
{ handle_exec(vi, ud, QUETZ_INSN_OTHER);       }

// Dispatch table indexed by QuetzInsnClass
static qemu_plugin_vcpu_udata_cb_t const g_exec_cbs[QUETZ_INSN_CLASS_COUNT] = {
    cb_exec_int,    // INT_MEM
    cb_exec_fp,     // FP_MEM
    cb_exec_vec,    // VEC_MEM
    cb_exec_icomp,  // INT_COMPUTE
    cb_exec_fcomp,  // FP_COMPUTE
    cb_exec_vcomp,  // VEC_COMPUTE
    cb_exec_branch, // BRANCH
    cb_exec_other,  // OTHER
};

// ---------------------------------------------------------------------------
// TB translation callback — instruments every instruction
// ---------------------------------------------------------------------------
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

        QuetzInsnClass cls;
        switch (g_isa) {
        case QUETZ_ISA_RISCV:
            cls = classify_riscv_insn(enc);
            break;
        case QUETZ_ISA_AARCH64:
            cls = classify_aarch64_insn(enc);
            break;
        default:
            // GENERIC: memory class deferred to handle_mem (size-based).
            // Non-memory instructions cannot be classified without a full
            // decoder; they are reported as OTHER.
            cls = QUETZ_INSN_OTHER;
            break;
        }

        // Memory callback: use the memory-specific class for load/store.
        // For GENERIC always use cb_mem_int as placeholder (overridden in
        // handle_mem at runtime).
        qemu_plugin_vcpu_mem_cb_t mem_cb;
        if (g_isa == QUETZ_ISA_GENERIC) {
            mem_cb = cb_mem_int;
        } else {
            mem_cb = (cls == QUETZ_INSN_FP_MEM || cls == QUETZ_INSN_FP_COMPUTE)
                         ? cb_mem_fp
                   : (cls == QUETZ_INSN_VEC_MEM || cls == QUETZ_INSN_VEC_COMPUTE)
                         ? cb_mem_vec
                   :       cb_mem_int;
        }

        qemu_plugin_register_vcpu_mem_cb(insn, mem_cb,
                                          QEMU_PLUGIN_CB_NO_REGS,
                                          QEMU_PLUGIN_MEM_RW, pc_ptr);
        qemu_plugin_register_vcpu_insn_exec_cb(insn, g_exec_cbs[cls],
                                                QEMU_PLUGIN_CB_NO_REGS,
                                                pc_ptr);
    }
}

// ---------------------------------------------------------------------------
// vCPU init callback
// ---------------------------------------------------------------------------
static void cb_vcpu_init(qemu_plugin_id_t /*id*/, unsigned int vcpu_index)
{
    if (vcpu_index < MAX_VCPUS) {
        g_mem_seen[vcpu_index].store(false, std::memory_order_relaxed);
        g_prev_cls[vcpu_index] = QUETZ_INSN_OTHER;
    }
}

// ---------------------------------------------------------------------------
// Plugin exit callback — send EXIT on every vCPU buffer
// ---------------------------------------------------------------------------
static void cb_atexit(qemu_plugin_id_t /*id*/, void* /*userdata*/)
{
    if (!g_tunnel) return;

#if QEMU_PLUGIN_VERSION >= 2
    int num_vcpus = qemu_plugin_num_vcpus();
#else
    int num_vcpus = qemu_plugin_n_vcpus();
#endif
    if (num_vcpus <= 0) num_vcpus = 1;

    QuetzCommand exit_cmd;
    exit_cmd.cmd  = QUETZ_CMD_EXIT;
    exit_cmd.size = 0;
    exit_cmd.pc   = 0;
    exit_cmd.addr = 0;

    for (int v = 0; v < num_vcpus && v < MAX_VCPUS; v++)
        g_tunnel->writeMessage((size_t)v, exit_cmd);
}

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------
extern "C"
QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id,
                        const qemu_info_t* info,
                        int argc, char** argv)
{
    if (info && info->target_name) {
        const char* t = info->target_name;
        if      (strncmp(t, "riscv",   5) == 0) g_isa = QUETZ_ISA_RISCV;
        else if (strncmp(t, "aarch64", 7) == 0) g_isa = QUETZ_ISA_AARCH64;
        else                                     g_isa = QUETZ_ISA_GENERIC;
        g_system_mode = info->system_emulation;
        fprintf(stderr,
            "[qemu_sst_plugin] Target: %s  ISA class: %s  system_mode: %d\n", t,
            (g_isa == QUETZ_ISA_RISCV)   ? "riscv" :
            (g_isa == QUETZ_ISA_AARCH64) ? "aarch64" : "generic",
            (int)g_system_mode);
    }

    for (unsigned i = 0; i < MAX_VCPUS; i++) {
        g_mem_seen[i].store(false, std::memory_order_relaxed);
        g_prev_cls[i] = QUETZ_INSN_OTHER;
    }

    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "shmname=",  8) == 0)
            g_shmem_name = std::string(argv[i] + 8);
        else if (strncmp(argv[i], "detailed=", 9) == 0)
            g_detailed = (argv[i][9] == '1');
    }

    if (g_detailed && g_isa == QUETZ_ISA_GENERIC) {
        fprintf(stderr,
            "[qemu_sst_plugin] WARNING: detailed instruction tracking "
            "requested but ISA '%s' has no instruction decoder.\n"
            "  All non-memory instructions will be reported as OTHER.\n"
            "  See the 'How to add a decoder for a new ISA' comment in\n"
            "  qemu_sst_plugin.cpp to add support for this target.\n",
            (info && info->target_name) ? info->target_name : "unknown");
    }

    if (g_shmem_name.empty()) {
        fprintf(stderr,
            "[qemu_sst_plugin] ERROR: no 'shmname=<name>' argument.\n");
        return 1;
    }

    g_shmchild = new SHMChild(g_shmem_name);
    g_tunnel   = g_shmchild->getTunnel();

    if (!g_tunnel) {
        fprintf(stderr,
            "[qemu_sst_plugin] ERROR: failed to attach to shmem '%s'.\n",
            g_shmem_name.c_str());
        return 1;
    }

    qemu_plugin_register_vcpu_init_cb(id,     cb_vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, cb_tb_trans);
    qemu_plugin_register_atexit_cb(id,        cb_atexit, nullptr);

    fprintf(stderr,
        "[qemu_sst_plugin] Attached to SST via shmem region '%s'%s.\n",
        g_shmem_name.c_str(),
        g_detailed ? " (detailed instruction tracking)" : "");
    return 0;
}
