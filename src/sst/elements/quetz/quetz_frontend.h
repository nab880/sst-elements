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



#ifndef _H_SST_QUETZ_FRONTEND
#define _H_SST_QUETZ_FRONTEND

#include <sst/core/component.h>
#include <sst/core/output.h>

#include <string>

#include "quetz_config.h"
#include "quetz_core_backend.h"

namespace SST {
namespace Quetz {

class QuetzFrontend {
public:
    virtual ~QuetzFrontend() = default;

    virtual QuetzCoreBackend* coreBackend() = 0;

    virtual void spawn(const QuetzConfig& cfg, bool detailed_tracking) = 0;
    virtual void waitForChildAttach() = 0;
    virtual void terminate() = 0;
    virtual void forceKill() = 0;

    virtual const std::string& shmemRegionName() const = 0;
};

} // namespace Quetz
} // namespace SST

#endif // _H_SST_QUETZ_FRONTEND
