// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2026, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include <sst_config.h>
#include "quetz_qemu_frontend.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace SST;
using namespace SST::Quetz;

QemuFrontend::QemuFrontend(ComponentId_t id, uint32_t vcpu_count,
                           uint32_t max_core_queue, SST::Output* out)
    : output_(out),
      // expectedChildren = 2:
      //   1) qemu_sst_plugin (calls tunnel->initialize, decrements)
      tunnelmgr_(new SST::Core::Interprocess::SHMParent<QuetzTunnel>(
          id, vcpu_count, max_core_queue, 2)),
      tunnel_(tunnelmgr_->getTunnel()),
      backend_(tunnel_),
      launcher_(out)
{}

QemuFrontend::~QemuFrontend() {
    // With expectedChildren=2 the plugin no longer unlinks the shm name.
    // Clean up explicitly so /dev/shm doesn't accumulate stale entries.
    std::string name = tunnelmgr_->getRegionName();
    delete tunnelmgr_;
    shm_unlink(name.c_str());
}

QuetzCoreBackend* QemuFrontend::coreBackend() {
    return &backend_;
}

void QemuFrontend::spawn(const QuetzConfig& cfg, bool detailed_tracking) {
    launcher_.spawn(cfg, tunnelmgr_->getRegionName(), detailed_tracking);
}

void QemuFrontend::waitForChildAttach() {
    // Poll child liveness while waiting: if QEMU dies before the plugin attaches
    // (bad plugin path, exec failure, plugin init error), a bare spin on the
    // attach flag would hang SST in init forever at 100% CPU.
    while (!tunnel_->sync().childAttached()) {
        int pstat = 0;
        pid_t rc = waitpid(launcher_.pid(), &pstat, WNOHANG);
        if (rc == launcher_.pid()) {
            if (WIFEXITED(pstat))
                output_->fatal(CALL_INFO, -1,
                    "QEMU exited with status %d before its SST plugin attached "
                    "(check the qemu_plugin path and the QEMU command line).\n",
                    WEXITSTATUS(pstat));
            if (WIFSIGNALED(pstat))
                output_->fatal(CALL_INFO, -1,
                    "QEMU was terminated by signal %d before its SST plugin "
                    "attached.\n", WTERMSIG(pstat));
            output_->fatal(CALL_INFO, -1,
                "QEMU stopped (pstat=%d) before its SST plugin attached.\n",
                pstat);
        }
        usleep(1000);
    }
}

void QemuFrontend::terminate() {
    launcher_.terminate();
}

void QemuFrontend::forceKill() {
    launcher_.forceKill();
}

const std::string& QemuFrontend::shmemRegionName() const {
    return tunnelmgr_->getRegionName();
}
