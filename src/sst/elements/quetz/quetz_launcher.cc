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
#include "quetz_launcher.h"

#include <cstdlib>
#include <inttypes.h>
#include <sstream>
#include <vector>
#include <signal.h>
#if defined(HAVE_SET_PTRACER)
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

using namespace SST;
using namespace SST::Quetz;

QemuLauncher::QemuLauncher(SST::Output* out)
    : output_(out), pid_(0)
{}

pid_t QemuLauncher::spawn(const QuetzConfig& cfg,
                          const std::string& shmem_region_name,
                          bool detailed_tracking) {
    std::string resolved_plugin = cfg.qemu_plugin;
    if (resolved_plugin.empty()) {
        std::string libexec =
            std::string(QEMU_PLUGIN_INSTALL_DIR) + "/libqemu_sst_plugin.so";
        if (access(libexec.c_str(), R_OK) == 0)
            resolved_plugin = libexec;
        else
            output_->fatal(CALL_INFO, -1,
                "qemu_plugin path not specified and could not find "
                "libqemu_sst_plugin.so in %s.\n",
                QEMU_PLUGIN_INSTALL_DIR);
    }

    std::string plugin_arg = resolved_plugin + ",shmname=" + shmem_region_name;
    if (detailed_tracking)
        plugin_arg += ",detailed=1";

    const char* mmio_start_env = getenv("QUETZ_MMIO_START");
    const char* mmio_end_env   = getenv("QUETZ_MMIO_END");
    const char* mmio_payload   = getenv("QUETZ_MMIO_PAYLOAD");
    uint64_t mmio_base = 0;
    uint64_t mmio_size = 0;
    if (mmio_payload && mmio_payload[0] == '1' && mmio_start_env && mmio_end_env) {
        mmio_base = strtoull(mmio_start_env, nullptr, 0);
        uint64_t mmio_end = strtoull(mmio_end_env, nullptr, 0);
        if (mmio_end >= mmio_base)
            mmio_size = mmio_end - mmio_base + 1;
        char plug_mmio[128];
        snprintf(plug_mmio, sizeof(plug_mmio),
            ",mmio_base=0x%" PRIx64 ",mmio_size=0x%" PRIx64, mmio_base, mmio_size);
        plugin_arg += plug_mmio;
    }

    // Optional SST-backed memory window (QUETZ_SST_WIN_START/END): a guest-phys
    // range whose accesses are trapped by a second sst-mmio-bridge aperture and
    // served synchronously by the SST memory hierarchy (router -> directory ->
    // MemController), so a device DMA and the guest see the same bytes. The
    // plugin is told the range so those accesses are NOT also trace-streamed.
    const char* win_start_env = getenv("QUETZ_SST_WIN_START");
    const char* win_end_env   = getenv("QUETZ_SST_WIN_END");
    uint64_t win_base = 0;
    uint64_t win_size = 0;
    if (win_start_env && win_end_env) {
        win_base = strtoull(win_start_env, nullptr, 0);
        uint64_t win_end = strtoull(win_end_env, nullptr, 0);
        if (win_end >= win_base)
            win_size = win_end - win_base + 1;
        char plug_win[128];
        snprintf(plug_win, sizeof(plug_win),
            ",win_base=0x%" PRIx64 ",win_size=0x%" PRIx64, win_base, win_size);
        plugin_arg += plug_win;
    }

    std::vector<std::string> argv_strs;
    argv_strs.reserve(8 + cfg.qemu_extra_args.size() + cfg.app_args.size());
    argv_strs.push_back(cfg.qemu_bin);
    for (const auto& a : cfg.qemu_extra_args) argv_strs.push_back(a);
    argv_strs.push_back("-plugin");
    argv_strs.push_back(plugin_arg);

    // Synchronous MMIO delivery (the value-returning doorbell path that drives
    // balar). System mode uses the sst-mmio-bridge device mapped into the machine
    // memory map; user mode (P6) has no device map, so QEMU reserves the aperture
    // PROT_NONE (target_mmap) and the linux-user SIGSEGV handler routes faults to
    // the same sync mailbox — both reach pollMmioSyncMailbox identically.
    if (mmio_size != 0 && cfg.system_mode) {
        char dev[256];
        snprintf(dev, sizeof(dev),
            "sst-mmio-bridge,shmname=%s,base=0x%" PRIx64 ",size=0x%" PRIx64 ",vcpu_id=0",
            shmem_region_name.c_str(), mmio_base, mmio_size);
        std::string dev_arg(dev);
        // SST-device IRQ injection (QUETZ_IRQ_LINES = lines to poll, 0/unset =
        // off): only the MMIO bridge polls the reverse mailbox — the optional
        // SST-window aperture below shares the shmem and must not double-poll.
        const char* irq_lines_env = getenv("QUETZ_IRQ_LINES");
        if (irq_lines_env && strtoul(irq_lines_env, nullptr, 0) > 0) {
            dev_arg += ",irq-count=";
            dev_arg += irq_lines_env;
            const char* irq_poll_env = getenv("QUETZ_IRQ_POLL_NS");
            if (irq_poll_env && irq_poll_env[0]) {
                dev_arg += ",irq-poll-ns=";
                dev_arg += irq_poll_env;
            }
            const char* intc_env = getenv("QUETZ_IRQ_INTC_TYPE");
            if (intc_env && intc_env[0]) {
                dev_arg += ",intc-type=";
                dev_arg += intc_env;
            }
        }
        argv_strs.push_back("-device");
        argv_strs.push_back(dev_arg);
    } else if (mmio_size != 0 && !cfg.system_mode) {
        char range[256];
        snprintf(range, sizeof(range),
            "shmname=%s,base=0x%" PRIx64 ",size=0x%" PRIx64 ",vcpu_id=0",
            shmem_region_name.c_str(), mmio_base, mmio_size);
        argv_strs.push_back("-sst-mmio-range");
        argv_strs.push_back(range);
    }

    if (win_size != 0) {
        if (!cfg.system_mode)
            output_->fatal(CALL_INFO, -1,
                "QUETZ_SST_WIN_START/END is only supported in system mode "
                "(the window is an sst-mmio-bridge device aperture).\n");
        char dev[256];
        snprintf(dev, sizeof(dev),
            "sst-mmio-bridge,shmname=%s,base=0x%" PRIx64 ",size=0x%" PRIx64 ",vcpu_id=0",
            shmem_region_name.c_str(), win_base, win_size);
        argv_strs.push_back("-device");
        argv_strs.push_back(dev);
    }

    if (cfg.system_mode && !cfg.system_mode_loader.empty())
        argv_strs.push_back(cfg.system_mode_loader);
    argv_strs.push_back(cfg.executable);
    for (const auto& a : cfg.app_args) argv_strs.push_back(a);

    {
        std::ostringstream cmdline;
        for (size_t k = 0; k < argv_strs.size(); k++) {
            if (k) cmdline << ' ';
            cmdline << argv_strs[k];
        }
        output_->verbose(CALL_INFO, 1, 0,
            "QEMU command: %s\n", cmdline.str().c_str());
    }

    pid_t child = fork();
    if (child < 0)
        output_->fatal(CALL_INFO, -1, "fork() failed: %s\n", strerror(errno));

    if (child != 0) {
        // Startup failure (bad qemu path etc.) is detected by
        // waitForChildAttach(), which polls waitpid(WNOHANG) while spinning
        // on the attach flag.
        pid_ = child;
        return pid_;
    }

#if defined(HAVE_SET_PTRACER)
    prctl(PR_SET_PTRACER, getppid(), 0, 0, 0);
#endif

    for (const auto& kv : cfg.extra_env)
        setenv(kv.first.c_str(), kv.second.c_str(), 1);

    if (!cfg.stdin_file.empty()) {
        if (!freopen(cfg.stdin_file.c_str(), "r", stdin)) _exit(1);
    }
    if (!cfg.stdout_file.empty()) {
        if (!freopen(cfg.stdout_file.c_str(), "w", stdout)) _exit(1);
    }
    if (!cfg.stderr_file.empty()) {
        if (!freopen(cfg.stderr_file.c_str(), "w", stderr)) _exit(1);
    }

    std::vector<char*> argv;
    argv.reserve(argv_strs.size() + 1);
    for (auto& s : argv_strs) argv.push_back(s.data());
    argv.push_back(nullptr);

    execvp(argv[0], argv.data());
    perror("execvp");
    _exit(127);
}

void QemuLauncher::terminate() {
    if (pid_ == 0)
        return;
    kill(pid_, SIGTERM);
    // Reap the child so it does not linger as a zombie for the rest of the
    // SST process lifetime. Give SIGTERM a bounded grace window, then
    // escalate to SIGKILL (which cannot be ignored) and reap for real.
    const int grace_ms = 2000;
    const int poll_ms  = 10;
    for (int waited = 0; waited < grace_ms; waited += poll_ms) {
        int pstat;
        pid_t rc = waitpid(pid_, &pstat, WNOHANG);
        if (rc == pid_ || (rc < 0 && errno == ECHILD)) {
            pid_ = 0;
            return;
        }
        usleep(poll_ms * 1000);
    }
    output_->verbose(CALL_INFO, 1, 0,
        "QEMU (pid %d) did not exit within %d ms of SIGTERM; sending SIGKILL.\n",
        (int)pid_, grace_ms);
    kill(pid_, SIGKILL);
    int pstat;
    waitpid(pid_, &pstat, 0);
    pid_ = 0;
}

void QemuLauncher::forceKill() {
    if (pid_ == 0)
        return;
    kill(pid_, SIGKILL);
    int pstat;
    waitpid(pid_, &pstat, 0);
    pid_ = 0;
}
