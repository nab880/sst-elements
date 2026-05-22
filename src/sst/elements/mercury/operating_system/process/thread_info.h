// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#pragma once

#include <mercury/operating_system/process/global.h>
#include <mercury/operating_system/process/tls.h>

#include <cstring>
#include <cstdio>
#include <cstdint>
#include <set>

extern "C" int sst_hg_global_stacksize;

namespace SST {
namespace Hg {

class ThreadInfo {
 public:
  // Populate the per-Thread TLS slot and run any pending init functions for
  // the new globals/TLS segments. `tls` is the Thread's owned SstHgThreadTls
  // struct (was previously stashed at the bottom of the coroutine stack via
  // the SP-alignment trick).
  static void registerUserSpaceVirtualThread(int phys_thread_id,
                                             SstHgThreadTls* tls,
                                             void* globalsMap, void* tlsMap,
                                             bool isAppStartup, bool isThreadStartup);

  static void deregisterUserSpaceVirtualThread(SstHgThreadTls* tls);

  static inline int currentPhysicalThreadId(){
    SstHgThreadTls* t = sst_hg_current_tls;
    return t ? t->thread_id : 0;
  }

};

} // end namespace Hg
} // end namespace SST
