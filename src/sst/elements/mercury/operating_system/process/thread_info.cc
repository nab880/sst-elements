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

#include <mercury/common/errors.h>
#include <mercury/components/operating_system.h>
#include <mercury/operating_system/process/thread_info.h>
#include <mercury/operating_system/process/app.h>
#include <mercury/operating_system/process/tls.h>
#include <mercury/operating_system/threading/thread_lock.h>
#include <mercury/operating_system/threading/stack_alloc.h>

#include <iostream>
#include <string.h>
#include <stdint.h>
#include <list>

namespace SST {
namespace Hg {

extern template class  HgBase<SST::Component>;
extern template class  HgBase<SST::SubComponent>;

static const int tls_sanity_check = 42042042;

static thread_lock globals_lock;

extern "C" void* sst_hg_alloc_stack(int sz, int /*md_sz*/)
{
  if (sz > SST::Hg::OperatingSystem::stacksize()){
    sst_hg_abort_printf("Cannot allocate stack larger than %d - requested %d",
                      SST::Hg::OperatingSystem::stacksize(), sz);
  }
  // The legacy SP-alignment trick reserved an SST_HG_TLS_OFFSET prefix in the
  // stack for per-coroutine TLS. With the pthread-local sst_hg_current_tls
  // mechanism that prefix is no longer needed, so md_sz is unrestricted.
  return SST::Hg::StackAlloc::alloc();
}

extern "C" void sst_hg_free_stack(void* /*ptr*/)
{
  abort("sst_hg_free_stack");
//  SST::Hg::StackAlloc::free(ptr);
}

void
ThreadInfo::registerUserSpaceVirtualThread(int phys_thread_id,
                                           SstHgThreadTls* tls,
                                           void* globalsMap, void* tlsMap,
                                           bool isAppStartup,
                                           bool isThreadStartup)
{
  // Populate the per-Thread TLS slot. This pointer is what the Mercury OS
  // layer installs into sst_hg_current_tls on every context switch into this
  // Thread; subsequent global/TLS lookups resolve through it.
  tls->thread_id      = phys_thread_id;
  tls->global_map     = globalsMap;
  tls->tls_map        = tlsMap;
  tls->implicit_state = nullptr;
  tls->sanity_check   = tls_sanity_check;

  globals_lock.lock();
  // Init functions for newly-active globals/TLS segments run on whatever
  // physical thread is calling us (typically the DES main thread during App
  // launch). They must read/write the new app's segment via the standard
  // accessor path, so temporarily install the new TLS pointer for the
  // duration of the call sequence, then restore. We save/restore rather than
  // unconditionally clearing to be safe under nested or recursive launches.
  SstHgThreadTls* saved = sst_hg_current_tls;
  sst_hg_current_tls = tls;

  if (globalsMap && isAppStartup){
    GlobalVariable::glblCtx.addActiveSegment(globalsMap);
    GlobalVariable::glblCtx.callInitFxns(globalsMap);
  }

  if (tlsMap && isThreadStartup){
    GlobalVariable::tlsCtx.addActiveSegment(tlsMap);
    GlobalVariable::tlsCtx.callInitFxns(tlsMap);
  }

  sst_hg_current_tls = saved;
  globals_lock.unlock();
}

void
ThreadInfo::deregisterUserSpaceVirtualThread(SstHgThreadTls* /*tls*/)
{
  // Active-segment teardown is intentionally a no-op today (matches legacy
  // commented-out behavior). Re-enabling it requires also removing the
  // segment from GlobalVariableContext::activeGlobalMaps_, which races with
  // in-flight accessors on other physical threads.
}

} // end namespace Hg
} // end namespace SST

