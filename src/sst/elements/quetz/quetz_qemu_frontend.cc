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

using namespace SST;
using namespace SST::Quetz;

QemuFrontend::QemuFrontend(ComponentId_t id, uint32_t vcpu_count,
                           uint32_t max_core_queue, SST::Output* out)
    : output_(out),
      tunnelmgr_(new SST::Core::Interprocess::SHMParent<QuetzTunnel>(
          id, vcpu_count, max_core_queue)),
      tunnel_(tunnelmgr_->getTunnel()),
      backend_(tunnel_),
      launcher_(out)
{}

QemuFrontend::~QemuFrontend() {
    delete tunnelmgr_;
}

QuetzCoreBackend* QemuFrontend::coreBackend() {
    return &backend_;
}

void QemuFrontend::spawn(const QuetzConfig& cfg, bool detailed_tracking) {
    launcher_.spawn(cfg, tunnelmgr_->getRegionName(), detailed_tracking);
}

void QemuFrontend::waitForChildAttach() {
    tunnel_->waitForChild();
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
