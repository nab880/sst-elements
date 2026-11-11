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

//#include <mercury/common/skeleton.h>
#include <mercury/common/timestamp.h>
#include <mercury/operating_system/libraries/unblock_event.h>
#include <mercury/components/operating_system_api.h>
#include <mercury/components/operating_system_impl.h>
#include <mercury/common/errors.h>
#include <mercury/common/null_buffer.h>
#include <mercury/libraries/compute/compute_api.h>
#include <mercury/operating_system/process/thread.h>
#include <mercury/operating_system/process/thread_info.h>
#include <cstdint>
#include <cstdlib>

namespace SST::Hg {

unsigned int
ssthg_sleep(unsigned int secs) {
    OperatingSystemAPI* cos = OperatingSystemImpl::currentOs();
    Thread* t = cos->activeThread();
    UnblockEvent* ev = new UnblockEvent(cos, t);
    cos->sendDelayedExecutionEvent(TimeDelta(secs, TimeDelta::one_second), ev);
    cos->block();
    return 0;
}

unsigned int
ssthg_usleep(unsigned int usecs) {
    OperatingSystemAPI* cos = OperatingSystemImpl::currentOs();
    Thread* t = cos->activeThread();
    UnblockEvent* ev = new UnblockEvent(cos, t);
    cos->sendDelayedExecutionEvent(TimeDelta(usecs, TimeDelta::one_microsecond), ev);
    cos->block();
    return 0;
}

unsigned int
ssthg_nanosleep(unsigned int nsecs) {
    OperatingSystemAPI* cos = OperatingSystemImpl::currentOs();
    Thread* t = cos->activeThread();
    UnblockEvent* ev = new UnblockEvent(cos, t);
    cos->sendDelayedExecutionEvent(TimeDelta(nsecs, TimeDelta::one_nanosecond), ev);
    cos->block();
    return 0;
}

} //end namespace SST::Hg

// Called in place of free() by code hgcc rewrote in skeletonize or shadowize
// mode (declared in skeleton.h).
extern "C" void
sst_hg_free(void* ptr)
{
  if (isNonNullBuffer(ptr)) {
    ::free(ptr);
  }
}

// The simulated thread of the application rank calling a hook. A hook can
// run before that thread starts, for example from a static initializer when
// the application library is loaded; there is no simulated time to charge
// then, so this aborts with a message instead. That code runs on the native
// stack, where the thread id currentOs() reads is stale memory, so check
// first that this is a Mercury thread's stack.
static SST::Hg::Thread*
hookThread(const char* hook)
{
  SST::Hg::Thread* thread = nullptr;
  if (SST::Hg::ThreadInfo::onUserSpaceStack()) {
    SST::Hg::OperatingSystemAPI* os = SST::Hg::OperatingSystemImpl::currentOs();
    thread = os ? os->activeThread() : nullptr;
  }
  if (!thread || thread->getState() != SST::Hg::Thread::ACTIVE) {
    sst_hg_abort_printf("%s called outside a running Mercury application "
                        "thread (for example from a static initializer)",
                        hook);
  }
  return thread;
}

// Charges modeled work on the calling rank's simulated thread through that
// app's ComputeLibrary. The concrete library lives in an ext library loaded
// with the app, so the call goes through the ComputeAPI interface defined
// here in libhg.
static void
computeDetailed(const char* hook, uint64_t flops, uint64_t intops,
                uint64_t bytes, int nthread)
{
  hookThread(hook)->getLibrary<SST::Hg::ComputeAPI>("ComputeLibrary")
      ->computeDetailed(flops, intops, bytes, nthread);
}

// Hooks emitted by hgcc's `#pragma sst compute` and, with a num_threads
// clause, `#pragma omp parallel` (declared in skeleton.h).
extern "C" void
sst_hg_compute_detailed_nthr(uint64_t flops, uint64_t intops, uint64_t bytes,
                             int nthread)
{
  if (nthread < 1) {
    sst_hg_abort_printf("sst_hg_compute_detailed_nthr: thread count %d; it "
                        "must be at least 1", nthread);
  }
  computeDetailed("sst_hg_compute_detailed_nthr", flops, intops, bytes,
                  nthread);
}

extern "C" void
sst_hg_compute_detailed(uint64_t flops, uint64_t intops, uint64_t bytes)
{
  computeDetailed("sst_hg_compute_detailed", flops, intops, bytes, 1);
}
