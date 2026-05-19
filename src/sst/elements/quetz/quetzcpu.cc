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

#include <sst_config.h>
#include "quetzcpu.h"

#include <sstream>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

using namespace SST;
using namespace SST::Quetz;
using namespace SST::Interfaces;

// ---------------------------------------------------------------------------
QuetzCPU::QuetzCPU(ComponentId_t id, Params& params)
    : Component(id),
      child_pid_(0),
      stop_ticking_(true),
      halted_count_(0)
{
    int verbosity = params.find<int>("verbose", 0);
    output_ = new SST::Output(
        "QuetzComponent[@f:@l:@p] ", verbosity, 0, SST::Output::STDOUT);

    output_->verbose(CALL_INFO, 1, 0, "Creating QuetzComponent...\n");

    vcpu_count_ = params.find<uint32_t>("vcpu_count", 1);
    output_->verbose(CALL_INFO, 1, 0,
        "Configuring for %" PRIu32 " vCPU(s).\n", vcpu_count_);

    uint32_t max_core_queue  = params.find<uint32_t>("maxcorequeue",      64);
    uint32_t max_pend_trans  = params.find<uint32_t>("maxtranscore",      16);
    uint32_t max_issue_cyc   = params.find<uint32_t>("maxissuepercycle",   1);
    uint64_t cache_line_sz   = params.find<uint64_t>("cachelinesize",     64);

    system_mode_        = params.find<bool>("system_mode", false);
    system_mode_loader_ = params.find<std::string>("system_mode_loader", "-kernel");
    qemu_bin_    = params.find<std::string>("qemu",        "qemu-riscv64");
    qemu_plugin_ = params.find<std::string>("qemu_plugin", "");
    executable_  = params.find<std::string>("executable",  "");

    if (executable_.empty())
        output_->fatal(CALL_INFO, -1,
            "No 'executable' parameter provided — nothing to run.\n");

    appargcount_ = params.find<uint32_t>("appargcount", 0);
    {
        char buf[256];
        for (uint32_t i = 0; i < appargcount_; i++) {
            snprintf(buf, sizeof(buf), "apparg%" PRIu32, i);
            app_args_.push_back(params.find<std::string>(buf, ""));
        }
    }

    stdin_file_  = params.find<std::string>("appstdin",  "");
    stdout_file_ = params.find<std::string>("appstdout", "");
    stderr_file_ = params.find<std::string>("appstderr", "");
    max_insts_         = params.find<uint64_t>("max_insts",      0ULL);
    check_addresses_   = params.find<uint32_t>("checkaddresses", 0);
    detailed_tracking_ = params.find<bool>("detailed_instruction_tracking", false);

    std::string qemu_args_str = params.find<std::string>("qemu_args", "");
    if (!qemu_args_str.empty()) {
        std::istringstream iss(qemu_args_str);
        std::string tok;
        while (iss >> tok)
            qemu_extra_args_.push_back(tok);
    }

    // ---- Architecture properties ------------------------------------------
    isa_str_     = params.find<std::string>("isa", "");
    has_fpu_     = params.find<bool>("has_fpu",    false);
    has_vector_  = params.find<bool>("has_vector", false);
    vector_vlen_ = params.find<uint32_t>("vector_vlen", 128);
    vector_elen_ = params.find<uint32_t>("vector_elen",  64);

    if (!isa_str_.empty())
        output_->verbose(CALL_INFO, 1, 0,
            "Modeled ISA: %s  fpu=%d  vector=%d  vlen=%" PRIu32
            "  elen=%" PRIu32 "\n",
            isa_str_.c_str(), (int)has_fpu_, (int)has_vector_,
            vector_vlen_, vector_elen_);

    // ---- Execution latency table ------------------------------------------
    // Memory-class execution latencies (cycles at queue head before cache issue).
    memset(exec_latency_, 0, sizeof(exec_latency_));
    exec_latency_[QUETZ_INSN_INT_MEM] = params.find<uint32_t>("exec_latency_int", 0);
    exec_latency_[QUETZ_INSN_FP_MEM]  = params.find<uint32_t>("exec_latency_fp",  0);
    exec_latency_[QUETZ_INSN_VEC_MEM] = params.find<uint32_t>("exec_latency_vec", 0);

    // Compute-class latencies (cycles at queue head before NOP is retired).
    memset(compute_latency_, 0, sizeof(compute_latency_));
    compute_latency_[QUETZ_INSN_INT_COMPUTE] = params.find<uint32_t>("compute_latency_int",    0);
    compute_latency_[QUETZ_INSN_FP_COMPUTE]  = params.find<uint32_t>("compute_latency_fp",     0);
    compute_latency_[QUETZ_INSN_VEC_COMPUTE] = params.find<uint32_t>("compute_latency_vec",    0);
    compute_latency_[QUETZ_INSN_BRANCH]      = params.find<uint32_t>("compute_latency_branch", 0);
    compute_latency_[QUETZ_INSN_OTHER]       = params.find<uint32_t>("compute_latency_other",  0);

    // compute_latency_{int,fp,vec,branch} require detailed instruction
    // tracking: without it the plugin cannot distinguish those classes and all
    // non-memory instructions arrive tagged as OTHER, making per-class latency
    // knobs ineffective.
    {
        bool needs_detailed =
            compute_latency_[QUETZ_INSN_INT_COMPUTE] ||
            compute_latency_[QUETZ_INSN_FP_COMPUTE]  ||
            compute_latency_[QUETZ_INSN_VEC_COMPUTE] ||
            compute_latency_[QUETZ_INSN_BRANCH];
        if (needs_detailed && !detailed_tracking_)
            output_->fatal(CALL_INFO, -1,
                "compute_latency_int/fp/vec/branch require "
                "detailed_instruction_tracking=1.  Without it the plugin "
                "cannot distinguish instruction classes and all non-memory "
                "instructions arrive as OTHER.  Either set "
                "detailed_instruction_tracking=1 or use "
                "compute_latency_other for a class-independent delay.\n");
    }

    // ---- Memory-map regions -----------------------------------------------
    uint32_t mmcount = params.find<uint32_t>("memmap_count", 0);
    {
        char buf[256];
        for (uint32_t r = 0; r < mmcount; r++) {
            MemRegion region;
            snprintf(buf, sizeof(buf), "memmap%" PRIu32 "_name",  r);
            region.name  = params.find<std::string>(buf, "");
            snprintf(buf, sizeof(buf), "memmap%" PRIu32 "_start", r);
            region.start = params.find<uint64_t>(buf, 0ULL);
            snprintf(buf, sizeof(buf), "memmap%" PRIu32 "_end",   r);
            region.end   = params.find<uint64_t>(buf, 0ULL);
            snprintf(buf, sizeof(buf), "memmap%" PRIu32 "_type",  r);
            std::string type = params.find<std::string>(buf, "memory");
            if (type == "filtered")
                region.type = MemRegionType::FILTERED;
            else if (type == "uart")
                region.type = MemRegionType::UART;
            else
                region.type = MemRegionType::MEMORY;

            snprintf(buf, sizeof(buf), "memmap%" PRIu32 "_uart_tx_offset", r);
            region.uart_tx_offset = params.find<uint32_t>(buf, 0);

            const char* type_str =
                (region.type == MemRegionType::FILTERED) ? "filtered" :
                (region.type == MemRegionType::UART)     ? "uart"     : "memory";
            output_->verbose(CALL_INFO, 1, 0,
                "MemMap region[%" PRIu32 "] '%s': "
                "0x%016" PRIx64 "-0x%016" PRIx64 " (%s)\n",
                r, region.name.c_str(), region.start, region.end, type_str);
            memmap_.push_back(region);
        }
    }

    // ---- Extra environment variables --------------------------------------
    int32_t env_count = params.find<int32_t>("envparamcount", -1);
    if (env_count > 0) {
        char buf[256];
        for (int32_t i = 0; i < env_count; i++) {
            snprintf(buf, sizeof(buf), "envparamname%" PRId32, i);
            std::string name = params.find<std::string>(buf, "");
            snprintf(buf, sizeof(buf), "envparamval%" PRId32, i);
            std::string val  = params.find<std::string>(buf, "");
            extra_env_.emplace_back(name, val);
        }
    }

    // -----------------------------------------------------------------------
    // Create the shared-memory tunnel (parent side)
    // -----------------------------------------------------------------------
    tunnelmgr_ = new SST::Core::Interprocess::SHMParent<QuetzTunnel>(
        id, vcpu_count_, max_core_queue);
    tunnel_ = tunnelmgr_->getTunnel();

    output_->verbose(CALL_INFO, 1, 0,
        "Shared-memory region: %s\n",
        tunnelmgr_->getRegionName().c_str());

    // -----------------------------------------------------------------------
    // Register clock
    // -----------------------------------------------------------------------
    std::string cpu_clock = params.find<std::string>("clock", "1GHz");
    output_->verbose(CALL_INFO, 1, 0,
        "Registering clock at %s.\n", cpu_clock.c_str());

    TimeConverter tc = registerClock(
        cpu_clock,
        new Clock::Handler<QuetzCPU, &QuetzCPU::tick>(this));

    // -----------------------------------------------------------------------
    // Load per-vCPU cores and memory interfaces
    // -----------------------------------------------------------------------
    output_->verbose(CALL_INFO, 1, 0, "Creating per-vCPU cores...\n");

    for (uint32_t i = 0; i < vcpu_count_; i++) {
        cores_.push_back(loadComponentExtension<QuetzCore>(
            tunnel_, i, max_pend_trans, output_,
            max_issue_cyc, max_core_queue,
            cache_line_sz, tc, params,
            exec_latency_, compute_latency_, memmap_,
            max_insts_, check_addresses_, detailed_tracking_));
    }

    // Load memory interfaces — named subcomponent slot preferred, then ports.
    SubComponentSlotInfo* mem_slot = getSubComponentSlotInfo("memory");
    if (mem_slot) {
        if (!mem_slot->isAllPopulated() ||
            (uint32_t)mem_slot->getMaxPopulatedSlotNumber() != vcpu_count_ - 1)
        {
            output_->fatal(CALL_INFO, -1,
                "Mismatch: 'memory' subcomponent slots must match vcpu_count "
                "(%" PRIu32 " vCPUs).\n", vcpu_count_);
        }
        for (uint32_t i = 0; i < vcpu_count_; i++) {
            auto* iface = mem_slot->create<StandardMem>(i,
                ComponentInfo::INSERT_STATS, tc,
                new StandardMem::Handler<QuetzCore,
                    &QuetzCore::handleMemResponse>(cores_[i]));
            mem_ifaces_.push_back(iface);
            cores_[i]->setMemLink(iface);
        }
    } else {
        char link_buf[128];
        for (uint32_t i = 0; i < vcpu_count_; i++) {
            Params par;
            snprintf(link_buf, sizeof(link_buf), "cache_link_%" PRIu32, i);
            par.insert("port", std::string(link_buf));
            auto* iface = loadAnonymousSubComponent<StandardMem>(
                "memHierarchy.standardInterface", "memory", i,
                ComponentInfo::SHARE_PORTS | ComponentInfo::INSERT_STATS,
                par, tc,
                new StandardMem::Handler<QuetzCore,
                    &QuetzCore::handleMemResponse>(cores_[i]));
            mem_ifaces_.push_back(iface);
            cores_[i]->setMemLink(iface);
        }
    }

    registerAsPrimaryComponent();
    primaryComponentDoNotEndSim();

    output_->verbose(CALL_INFO, 1, 0,
        "QuetzComponent initialization complete.\n");
}

// ---------------------------------------------------------------------------
QuetzCPU::~QuetzCPU() {
    delete tunnelmgr_;
    delete output_;
}

// ---------------------------------------------------------------------------
void QuetzCPU::init(unsigned int phase) {
    if (phase == 0) {
        output_->verbose(CALL_INFO, 1, 0,
            "Phase 0 init: launching QEMU child process.\n");
        child_pid_ = 0;
        launchQEMU();
        output_->verbose(CALL_INFO, 1, 0,
            "Waiting for QEMU plugin to attach...\n");
        tunnel_->waitForChild();
        output_->verbose(CALL_INFO, 1, 0, "Plugin attached!\n");
        stop_ticking_ = false;
    }

    for (uint32_t i = 0; i < vcpu_count_; i++)
        mem_ifaces_[i]->init(phase);
}

// ---------------------------------------------------------------------------
void QuetzCPU::finish() {
    output_->verbose(CALL_INFO, 1, 0,
        "QuetzComponent finishing at %" PRIu64 " ns.\n",
        (uint64_t)getCurrentSimTimeNano());

    for (uint32_t i = 0; i < vcpu_count_; i++)
        cores_[i]->finishCore();

    if (child_pid_ != 0)
        kill(child_pid_, SIGTERM);
}

// ---------------------------------------------------------------------------
void QuetzCPU::emergencyShutdown() {
    if (child_pid_ != 0)
        kill(child_pid_, SIGKILL);
    delete tunnelmgr_;
    tunnelmgr_ = nullptr;
}

// ---------------------------------------------------------------------------
// Halt quorum: the simulation ends only when EVERY vCPU has halted AND every
// vCPU has drained its in-flight memory transactions.  This prevents a single
// thread's early EXIT from terminating an MP run while other threads are still
// pumping work through the shared-memory tunnel.
//
// Implementation note: halted_count_ is incremented exactly once per vCPU on
// the rising edge of isCoreHalted(); the per-core halted_ flag is monotonic
// (set once on EXIT or max_insts, never cleared) so the edge fires at most
// once per vCPU and halted_count_ never exceeds vcpu_count_.
bool QuetzCPU::tick(SST::Cycle_t /*cycle*/) {
    tunnel_->updateTime(getCurrentSimTimeNano());
    tunnel_->incrementCycles();

    for (uint32_t i = 0; i < vcpu_count_; i++) {
        bool was_halted = cores_[i]->isCoreHalted();
        cores_[i]->tick();
        if (!was_halted && cores_[i]->isCoreHalted())
            halted_count_++;
    }

    if (halted_count_ < vcpu_count_)
        return false;

    // All vCPUs halted — wait for pending transactions to drain before
    // releasing primaryComponentOKToEndSim, otherwise late responses arrive
    // on a torn-down component.
    for (uint32_t i = 0; i < vcpu_count_; i++)
        if (cores_[i]->pendingCount() > 0) return false;

    if (!stop_ticking_) {
        output_->verbose(CALL_INFO, 1, 0,
            "All %" PRIu32 " vCPUs halted and drained — ending simulation.\n",
            vcpu_count_);
        primaryComponentOKToEndSim();
        stop_ticking_ = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fork and exec QEMU
// ---------------------------------------------------------------------------
void QuetzCPU::launchQEMU() {
    // Resolve plugin path
    if (qemu_plugin_.empty()) {
        std::string libexec =
            std::string(QEMU_PLUGIN_INSTALL_DIR) + "/libqemu_sst_plugin.so";
        if (access(libexec.c_str(), R_OK) == 0)
            qemu_plugin_ = libexec;
        else
            output_->fatal(CALL_INFO, -1,
                "qemu_plugin path not specified and could not find "
                "libqemu_sst_plugin.so in %s.\n",
                QEMU_PLUGIN_INSTALL_DIR);
    }

    // Build argv.
    //
    // User mode:   qemu-<arch>  [qemu_args] -plugin <...>  <exe>  [args]
    // System mode: qemu-system-<arch>  [qemu_args] -plugin <...>  -kernel <exe>  [args]
    std::string shmem_name = tunnelmgr_->getRegionName();
    std::string plugin_arg = qemu_plugin_ + ",shmname=" + shmem_name;
    if (detailed_tracking_)
        plugin_arg += ",detailed=1";

    uint32_t max_argc = 1
        + (uint32_t)qemu_extra_args_.size()
        + 2          // -plugin <arg>
        + 2          // optional: -kernel <exe>
        + appargcount_
        + 1;         // NULL terminator
    char** argv = (char**)malloc(sizeof(char*) * max_argc);
    uint32_t ai = 0;

    auto push = [&](const std::string& s) { argv[ai++] = strdup(s.c_str()); };

    push(qemu_bin_);
    for (const auto& a : qemu_extra_args_) push(a);
    push("-plugin");
    push(plugin_arg);
    if (system_mode_ && !system_mode_loader_.empty())
        push(system_mode_loader_);
    push(executable_);
    for (const auto& arg : app_args_) push(arg);
    argv[ai] = nullptr;

    output_->verbose(CALL_INFO, 1, 0,
        "QEMU command: %s %s-plugin %s %s%s ...\n",
        qemu_bin_.c_str(),
        qemu_extra_args_.empty() ? "" : "[extra_args] ",
        qemu_plugin_.c_str(),
        (system_mode_ && !system_mode_loader_.empty())
            ? (system_mode_loader_ + " ").c_str() : "",
        executable_.c_str());

    pid_t child = fork();
    if (child < 0)
        output_->fatal(CALL_INFO, -1, "fork() failed: %s\n", strerror(errno));

    if (child != 0) {
        child_pid_ = child;
        int pstat;
        pid_t rc = waitpid(child, &pstat, WNOHANG);
        if (rc > 0) {
            if (WIFEXITED(pstat))
                output_->fatal(CALL_INFO, -1,
                    "QEMU exited immediately with status %d.\n",
                    WEXITSTATUS(pstat));
            else if (WIFSIGNALED(pstat))
                output_->fatal(CALL_INFO, -1,
                    "QEMU terminated by signal %d.\n", WTERMSIG(pstat));
            else
                output_->fatal(CALL_INFO, -1,
                    "QEMU failed to start (pstat=%d).\n", pstat);
        }
        for (uint32_t i = 0; i < ai; i++) free(argv[i]);
        free(argv);
        return;
    }

    // Child — set up environment then exec QEMU
#if defined(HAVE_SET_PTRACER)
    prctl(PR_SET_PTRACER, getppid(), 0, 0, 0);
#endif

    for (const auto& kv : extra_env_)
        setenv(kv.first.c_str(), kv.second.c_str(), 1);

    if (!stdin_file_.empty()) {
        if (!freopen(stdin_file_.c_str(), "r", stdin)) _exit(1);
    }
    if (!stdout_file_.empty()) {
        if (!freopen(stdout_file_.c_str(), "w", stdout)) _exit(1);
    }
    if (!stderr_file_.empty()) {
        if (!freopen(stderr_file_.c_str(), "w", stderr)) _exit(1);
    }

    execvp(argv[0], argv);
    perror("execvp");
    _exit(127);
}
