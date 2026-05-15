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
 * quetzcpu.h — top-level SST Component for QEMU-based CPU simulation.
 *
 * QuetzComponent acts as an Ariel-like front-end that:
 *   1. Forks a QEMU user-mode process with the SST TCG plugin loaded.
 *   2. Receives memory-access events from the plugin via shared memory.
 *   3. Forwards those events to memHierarchy (or any StandardMem backend).
 *
 * Designed for RISC-V (qemu-riscv64) but works with any QEMU user-mode
 * target, making it easy to model extended ISAs without a full cycle-accurate
 * pipeline simulator.
 */

#ifndef _H_SST_QUETZ_CPU
#define _H_SST_QUETZ_CPU

#include <sst/core/sst_config.h>
#include <sst/core/component.h>
#include <sst/core/interfaces/stdMem.h>
#include <sst/core/output.h>
#include <sst/core/params.h>
#include <sst/core/interprocess/shmparent.h>

#include <stdint.h>
#include <sys/types.h>
#include <string>
#include <vector>

#include "quetz_shmem.h"
#include "quetzcore.h"

namespace SST {
namespace Quetz {

class QuetzCPU : public SST::Component {
public:

    // -----------------------------------------------------------------------
    // SST Element Library Interface (ELI)
    // -----------------------------------------------------------------------
    SST_ELI_REGISTER_COMPONENT(
        QuetzCPU,
        "quetz",
        "QuetzComponent",
        SST_ELI_ELEMENT_VERSION(1, 0, 0),
        "QEMU TCG-plugin-based CPU model: traces memory accesses from a "
        "guest binary and drives memHierarchy for cache/memory simulation. "
        "Designed for RISC-V extended-ISA research (RVV, custom extensions); "
        "works with any qemu-user-mode target.",
        COMPONENT_CATEGORY_PROCESSOR
    )

    SST_ELI_DOCUMENT_PARAMS(
        { "verbose",
          "Verbosity level (0 = quiet, higher = more output).", "0" },
        { "clock",
          "CPU clock rate used to drive the SST time-base.", "1GHz" },
        { "vcpu_count",
          "Number of guest vCPUs / hardware threads. One shared-memory "
          "buffer is allocated per vCPU.  For single-threaded binaries "
          "this is 1; for OpenMP / pthread binaries set it to the thread "
          "count.", "1" },
        { "maxcorequeue",
          "Maximum depth of the per-vCPU event staging queue.", "64" },
        { "maxtranscore",
          "Maximum number of in-flight memory transactions per vCPU.", "16" },
        { "maxissuepercycle",
          "Maximum memory requests issued to the cache per cycle per vCPU.",
          "1" },
        { "cachelinesize",
          "Cache line size in bytes (used when splitting wide accesses).",
          "64" },
        { "executable",
          "Path to the guest binary to run under QEMU.", "" },
        { "qemu",
          "Path to the QEMU user-mode binary "
          "(e.g. /usr/bin/qemu-riscv64 or qemu-riscv64 if on PATH).",
          "qemu-riscv64" },
        { "qemu_plugin",
          "Path to the compiled SST QEMU plugin shared library "
          "(libqemu_sst_plugin.so).  If empty, the component looks for it "
          "in the directory returned by sst-config --prefix/libexec/.", "" },
        { "qemu_args",
          "Space-separated extra QEMU arguments inserted before -plugin "
          "(e.g. \"-L /opt/riscv/sysroot\" for a custom library path).",
          "" },
        { "appargcount",
          "Number of arguments to pass to the guest binary.", "0" },
        { "apparg%(appargcount)d",
          "Argument N for the guest binary.", "" },
        { "envparamcount",
          "Number of extra environment variables for the guest process "
          "(-1 = inherit the full SST environment).", "-1" },
        { "envparamname%(envparamcount)d",
          "Name of extra environment variable N.", "" },
        { "envparamval%(envparamcount)d",
          "Value of extra environment variable N.", "" },
        { "appstdin",
          "Redirect guest stdin from this file (default: inherit).", "" },
        { "appstdout",
          "Redirect guest stdout to this file (default: inherit).", "" },
        { "appstderr",
          "Redirect guest stderr to this file (default: inherit).", "" },
        { "max_insts",
          "Halt simulation after this many guest instructions per vCPU "
          "(0 = run to completion).", "0" },
        { "checkaddresses",
          "If 1, emit a warning when a single access is wider than one cache "
          "line (the 2-part split may be insufficient for those accesses).",
          "0" },
        { "detailed_instruction_tracking",
          "If 1, enable per-class non-memory instruction counting "
          "(int_compute, fp_compute, vec_compute, branch statistics). "
          "Requires RISC-V or AArch64 guest; other ISAs report all "
          "non-memory instructions as OTHER and a warning is emitted. "
          "Default 0 (all four detailed stats remain zero).",
          "0" },

        // ---- Architecture properties ---------------------------------------
        { "isa",
          "ISA string describing the modeled architecture (e.g. rv64gcv). "
          "Informational only — logged at startup.",
          "" },
        { "has_fpu",
          "Set to 1 if the modeled architecture has a floating-point unit. "
          "Logged at startup for documentation; QEMU user-mode enables FP "
          "automatically when the binary uses FP instructions.", "0" },
        { "has_vector",
          "Set to 1 if the modeled architecture has a vector / SIMD unit. "
          "Logged at startup for documentation.", "0" },
        { "vector_vlen",
          "Vector register length in bits (RISC-V VLEN). "
          "Logged at startup; QEMU user-mode picks the VLEN from the binary "
          "and host CPU capabilities.", "128" },
        { "vector_elen",
          "Maximum vector element width in bits (RISC-V ELEN). "
          "Logged at startup.", "64" },

        // ---- Execution latency per instruction class -----------------------
        // Extra cycles a memory command waits at the head of the issue queue
        // before the request is forwarded to the cache.  0 = no extra delay.
        { "exec_latency_int",
          "Extra pipeline cycles before an integer load/store issues to cache.",
          "0" },
        { "exec_latency_fp",
          "Extra pipeline cycles before a scalar FP load/store issues to cache.",
          "0" },
        { "exec_latency_vec",
          "Extra pipeline cycles before a vector load/store issues to cache.",
          "0" },

        // ---- Compute latency per instruction class -------------------------
        // Extra cycles a non-memory instruction occupies at the head of the
        // issue queue before it is retired.  Models functional-unit execution
        // latency for compute-bound code.  0 = retire immediately (default).
        //
        // IMPORTANT: compute_latency_{int,fp,vec,branch} have no effect unless
        // detailed_instruction_tracking=1 and the guest ISA has a full
        // instruction decoder (RISC-V or AArch64).  Setting a non-zero value
        // without detailed_instruction_tracking=1 is a fatal configuration
        // error; all non-memory instructions will be classified as OTHER on
        // ISAs without a decoder, so compute_latency_other is the only
        // latency knob that works for those targets.
        { "compute_latency_int",
          "Extra cycles an integer compute instruction occupies the issue queue.",
          "0" },
        { "compute_latency_fp",
          "Extra cycles a scalar FP compute instruction occupies the issue queue.",
          "0" },
        { "compute_latency_vec",
          "Extra cycles a vector/SIMD compute instruction occupies the issue queue.",
          "0" },
        { "compute_latency_branch",
          "Extra cycles a branch/jump/call/return occupies the issue queue.",
          "0" },
        { "compute_latency_other",
          "Extra cycles an unclassified (OTHER) non-memory instruction occupies "
          "the issue queue.  Applies to all ISAs including GENERIC targets.",
          "0" },

        // ---- Memory-map regions --------------------------------------------
        // Regions partition the guest address space.  Accesses to 'filtered'
        // regions are counted but NOT forwarded to the memory hierarchy —
        // useful for excluding Linux VDSO, kernel addresses, or MMIO from
        // cache statistics.  Accesses outside all defined regions are always
        // forwarded.
        { "memmap_count",
          "Number of named address-range regions (0 = all addresses forwarded).",
          "0" },
        { "memmap%(memmap_count)d_name",
          "Name of memory-map region N (used in log output).", "" },
        { "memmap%(memmap_count)d_start",
          "Inclusive start address of region N (decimal or 0x hex).", "0" },
        { "memmap%(memmap_count)d_end",
          "Inclusive end address of region N (decimal or 0x hex).", "0" },
        { "memmap%(memmap_count)d_type",
          "Region type: 'memory' (forward to hierarchy) or "
          "'filtered' (drop, count in filtered_reads/filtered_writes).",
          "memory" }
    )

    SST_ELI_DOCUMENT_PORTS(
        { "cache_link_%(vcpu_count)d",
          "Per-vCPU link to the first-level cache or memory hierarchy.", {} }
    )

    SST_ELI_DOCUMENT_STATISTICS(
        { "read_requests",
          "Total guest memory reads forwarded to the cache hierarchy.",
          "requests", 1 },
        { "write_requests",
          "Total guest memory writes forwarded to the cache hierarchy.",
          "requests", 1 },
        { "read_latency",
          "Cumulative round-trip latency of read requests (cycles).",
          "cycles", 1 },
        { "write_latency",
          "Cumulative round-trip latency of write requests (cycles).",
          "cycles", 1 },
        { "read_request_sizes",
          "Size distribution of read requests in bytes.",
          "bytes", 1 },
        { "write_request_sizes",
          "Size distribution of write requests in bytes.",
          "bytes", 1 },
        { "split_read_requests",
          "Read requests split across a cache-line boundary.",
          "requests", 1 },
        { "split_write_requests",
          "Write requests split across a cache-line boundary.",
          "requests", 1 },
        { "no_ops",
          "Instructions observed with no memory side-effect.",
          "instructions", 1 },
        { "cycles",
          "Simulated clock cycles.",
          "cycles", 1 },
        { "active_cycles",
          "Cycles during which at least one memory operation was issued.",
          "cycles", 1 },
        { "instruction_count",
          "Total guest instructions observed.",
          "instructions", 1 },
        { "filtered_reads",
          "Reads to filtered address regions — not forwarded to cache.",
          "requests", 1 },
        { "filtered_writes",
          "Writes to filtered address regions — not forwarded to cache.",
          "requests", 1 },
        { "stall_cycles",
          "Cycles stalled for execution-unit latency before a memory request "
          "reaches the cache.",
          "cycles", 1 },
        { "compute_stall_cycles",
          "Cycles stalled for compute-unit latency (non-memory instructions "
          "held at the head of the issue queue).",
          "cycles", 1 },
        { "int_compute",
          "Non-memory integer ALU instructions (zero when "
          "detailed_instruction_tracking=0).",
          "instructions", 1 },
        { "fp_compute",
          "Non-memory floating-point arithmetic instructions (zero when "
          "detailed_instruction_tracking=0).",
          "instructions", 1 },
        { "vec_compute",
          "Non-memory vector / SIMD arithmetic instructions (zero when "
          "detailed_instruction_tracking=0).",
          "instructions", 1 },
        { "branch",
          "Branch, jump, call, and return instructions (zero when "
          "detailed_instruction_tracking=0).",
          "instructions", 1 }
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        { "memory",
          "StandardMem interface to the memory hierarchy "
          "(one slot per vCPU, indexed 0..vcpu_count-1).",
          "SST::Interfaces::StandardMem" }
    )

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    QuetzCPU(ComponentId_t id, Params& params);
    ~QuetzCPU();

    void init(unsigned int phase)   override;
    void setup()                    override {}
    void finish()                   override;
    void emergencyShutdown()        override;
    bool tick(SST::Cycle_t cycle);

private:
    void launchQEMU();

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------
    uint32_t    vcpu_count_;
    std::string qemu_bin_;
    std::string qemu_plugin_;
    std::string executable_;
    uint32_t    appargcount_;
    std::vector<std::string> app_args_;
    std::vector<std::string> qemu_extra_args_;

    std::string stdin_file_;
    std::string stdout_file_;
    std::string stderr_file_;

    uint64_t    max_insts_;
    uint32_t    check_addresses_;
    bool        detailed_tracking_;

    std::string isa_str_;
    bool        has_fpu_;
    bool        has_vector_;
    uint32_t    vector_vlen_;
    uint32_t    vector_elen_;

    uint32_t    exec_latency_[QUETZ_INSN_CLASS_COUNT];
    uint32_t    compute_latency_[QUETZ_INSN_CLASS_COUNT];

    std::vector<MemRegion>                            memmap_;
    std::vector<std::pair<std::string,std::string>>   extra_env_;

    // -----------------------------------------------------------------------
    // Runtime state
    // -----------------------------------------------------------------------
    SST::Output* output_;

    SST::Core::Interprocess::SHMParent<QuetzTunnel>* tunnelmgr_;
    QuetzTunnel*                                      tunnel_;

    pid_t  child_pid_;
    bool   stop_ticking_;

    std::vector<QuetzCore*>                     cores_;
    std::vector<SST::Interfaces::StandardMem*>  mem_ifaces_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_CPU
