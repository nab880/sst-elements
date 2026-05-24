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

    std::vector<std::string> argv_strs;
    argv_strs.reserve(8 + cfg.qemu_extra_args.size() + cfg.app_args.size());
    argv_strs.push_back(cfg.qemu_bin);
    for (const auto& a : cfg.qemu_extra_args) argv_strs.push_back(a);
    argv_strs.push_back("-plugin");
    argv_strs.push_back(plugin_arg);
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
        pid_ = child;
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
    if (pid_ != 0)
        kill(pid_, SIGTERM);
}

void QemuLauncher::forceKill() {
    if (pid_ != 0)
        kill(pid_, SIGKILL);
}
