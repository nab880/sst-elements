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

#include <sst/core/params.h>
#include <sst/core/unitAlgebra.h>
#include <mercury/common/errors.h>
#include <mercury/common/timestamp.h>
#include <mercury/components/operating_system_api.h>
#include <mercury/libraries/gpu/gpu_library.h>
#include <mercury/libraries/gpu/hg_cuda.h>
#include <mercury/operating_system/process/app.h>

#include <iostream>

static_assert(SST_HG_CUDA_ABI_VERSION == 1,
              "GpuLibrary built against an unexpected sst_hg_cuda ABI version");

namespace {
// Reserved cookie range base (CUDA_PLAN.md D6). 256-byte aligned, bump
// allocated, never reused.
const uint64_t kCookieBase = 0x4000000000000000ull;
} // namespace

namespace SST {
namespace Hg {

GpuLibrary::GpuLibrary(SST::Params& params, App* parent)
    : Library(params, parent),
      total_gpu_time_(0.0),
      cookie_next_(kCookieBase),
      cookie_end_(kCookieBase),
      next_handle_(1),
      launch_count_(0),
      memcpy_count_(0)
{
  // Fixed-cost stub timing (CUDA-IMPL-PLAN.md P2 Track 3); values carry units,
  // e.g. gpu_kernel_time = "10us". getValue().toDouble() yields seconds.
  kernel_time_ =
      params.find<SST::UnitAlgebra>("gpu_kernel_time", "10us").getValue().toDouble();
  memcpy_time_ =
      params.find<SST::UnitAlgebra>("gpu_memcpy_time", "5us").getValue().toDouble();
  launch_overhead_ =
      params.find<SST::UnitAlgebra>("gpu_kernel_launch_overhead", "1us").getValue().toDouble();
}

GpuLibrary::~GpuLibrary()
{
  // Library::finish() is never invoked by the OS, but app teardown deletes
  // every library (app.cc), so the per-rank summary is emitted here. The test
  // greps this line for a nonzero total_gpu_time.
  parent()->coutStream()
      << "[gpu] rank summary: launches=" << launch_count_
      << " memcpys=" << memcpy_count_
      << " total_gpu_time=" << total_gpu_time_ << " s" << std::endl;
}

void
GpuLibrary::chargeTime(double seconds)
{
  if (seconds <= 0.0) return;
  total_gpu_time_ += seconds;
  parent()->os()->blockTimeout(TimeDelta(seconds));
}

void*
GpuLibrary::malloc(uint64_t bytes)
{
  void* p = reinterpret_cast<void*>(cookie_next_);
  uint64_t sz = (bytes + 255u) & ~static_cast<uint64_t>(255u);
  cookie_next_ += sz ? sz : 256u;
  cookie_end_ = cookie_next_;
  return p;
}

void
GpuLibrary::free(void* /*dptr*/)
{
}

int
GpuLibrary::isDevicePtr(const void* p)
{
  uint64_t v = reinterpret_cast<uint64_t>(p);
  return v >= kCookieBase && v < cookie_end_;
}

void
GpuLibrary::memcpy(void* /*dst*/, const void* /*src*/, uint64_t /*bytes*/,
                   int /*kind*/, void* /*stream*/)
{
  ++memcpy_count_;
  chargeTime(memcpy_time_);
}

void
GpuLibrary::launch(const char* /*kernelName*/,
                   uint32_t /*gx*/, uint32_t /*gy*/, uint32_t /*gz*/,
                   uint32_t /*bx*/, uint32_t /*by*/, uint32_t /*bz*/,
                   uint64_t /*shmemBytes*/, void* /*stream*/,
                   uint64_t /*flops*/, uint64_t /*intops*/,
                   uint64_t /*bytesRead*/, uint64_t /*bytesWritten*/)
{
  ++launch_count_;
  chargeTime(launch_overhead_ + kernel_time_);
}

void*
GpuLibrary::streamCreate()
{
  return reinterpret_cast<void*>(next_handle_++);
}

void
GpuLibrary::streamDestroy(void* /*s*/)
{
}

void
GpuLibrary::streamSync(void* /*s*/)
{
}

void*
GpuLibrary::eventCreate()
{
  return reinterpret_cast<void*>(next_handle_++);
}

void
GpuLibrary::eventRecord(void* /*evt*/, void* /*stream*/)
{
}

void
GpuLibrary::eventSync(void* /*evt*/)
{
}

float
GpuLibrary::eventElapsedMs(void* /*start*/, void* /*stop*/)
{
  return 0.0f;
}

void
GpuLibrary::deviceSync()
{
}

int
GpuLibrary::getDeviceCount()
{
  return 1;
}

void
GpuLibrary::setDevice(int /*dev*/)
{
}

} // end namespace Hg
} // end namespace SST
