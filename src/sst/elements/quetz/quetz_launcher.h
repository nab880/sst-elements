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

#ifndef _H_SST_QUETZ_LAUNCHER
#define _H_SST_QUETZ_LAUNCHER

#include <sst/core/output.h>

#include <sys/types.h>
#include <string>

#include "quetz_config.h"

namespace SST {
namespace Quetz {

class QemuLauncher {
public:
    explicit QemuLauncher(SST::Output* out);

    pid_t spawn(const QuetzConfig& cfg, const std::string& shmem_region_name,
                bool detailed_tracking);
    void  terminate();
    void  forceKill();

    pid_t pid() const { return pid_; }

private:
    SST::Output* output_;
    pid_t        pid_;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_LAUNCHER
