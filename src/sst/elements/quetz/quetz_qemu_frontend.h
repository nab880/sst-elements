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

#ifndef _H_SST_QUETZ_QEMU_FRONTEND
#define _H_SST_QUETZ_QEMU_FRONTEND

#include <sst/core/interprocess/shmparent.h>

#include <string>

#include "quetz_frontend.h"
#include "quetz_launcher.h"
#include "quetz_shmem.h"

namespace SST {
namespace Quetz {

class QemuFrontend : public QuetzFrontend {
public:
    QemuFrontend(ComponentId_t id, uint32_t vcpu_count,
                 uint32_t max_core_queue, SST::Output* out);
    ~QemuFrontend() override;

    QuetzCoreBackend* coreBackend() override;

    void spawn(const QuetzConfig& cfg, bool detailed_tracking) override;
    void waitForChildAttach() override;
    void terminate() override;
    void forceKill() override;

    const std::string& shmemRegionName() const override;

    QuetzTunnel* tunnel() { return tunnel_; }

private:
    SST::Output*                                          output_;
    SST::Core::Interprocess::SHMParent<QuetzTunnel>*      tunnelmgr_;
    QuetzTunnel*                                          tunnel_;
    QuetzTunnelBackend                                    backend_;
    QemuLauncher                                          launcher_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_QEMU_FRONTEND
