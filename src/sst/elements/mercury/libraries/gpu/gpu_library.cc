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

#include <algorithm>
#include <iostream>

static_assert(SST_HG_CUDA_ABI_VERSION == 1,
              "GpuLibrary built against an unexpected sst_hg_cuda ABI version");

namespace {
// Reserved cookie range base (CUDA_PLAN.md D6). 256-byte aligned, bump
// allocated, never reused.
const uint64_t kCookieBase = 0x4000000000000000ull;

// cudaMemcpyKind (driver_types.h): only DeviceToDevice stays on the GPU; the
// rest cross PCIe.
const int kCudaMemcpyDeviceToDevice = 3;
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
  // Roofline params (CUDA_PLAN.md §4). Bandwidths/latencies carry units
  // (e.g. "900GB/s", "1us"); peak_flops is a bare flop/s rate (no SI unit for
  // "flop"), default 10 TFLOP/s.
  peak_flops_ = params.find<double>("gpu_peak_flops", 1.0e13);
  mem_bandwidth_ =
      params.find<SST::UnitAlgebra>("gpu_mem_bandwidth", "900GB/s").getValue().toDouble();
  pcie_latency_ =
      params.find<SST::UnitAlgebra>("pcie_latency", "1us").getValue().toDouble();
  pcie_bandwidth_ =
      params.find<SST::UnitAlgebra>("pcie_bandwidth", "16GB/s").getValue().toDouble();
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

double
GpuLibrary::kernelTime(uint64_t totalThreads, uint64_t flopsPerThread,
                       uint64_t intopsPerThread, uint64_t bytesReadPerThread,
                       uint64_t bytesWrittenPerThread) const
{
  double threads = static_cast<double>(totalThreads);
  double flopsTotal =
      static_cast<double>(flopsPerThread + intopsPerThread) * threads;
  double bytesTotal =
      static_cast<double>(bytesReadPerThread + bytesWrittenPerThread) * threads;
  double computeT = peak_flops_ > 0.0 ? flopsTotal / peak_flops_ : 0.0;
  double memT = mem_bandwidth_ > 0.0 ? bytesTotal / mem_bandwidth_ : 0.0;
  return launch_overhead_ + std::max(computeT, memT);
}

double
GpuLibrary::transferTime(uint64_t bytes, int kind) const
{
  double bw = (kind == kCudaMemcpyDeviceToDevice) ? mem_bandwidth_ : pcie_bandwidth_;
  double lat = (kind == kCudaMemcpyDeviceToDevice) ? 0.0 : pcie_latency_;
  double bwTime = bw > 0.0 ? static_cast<double>(bytes) / bw : 0.0;
  return lat + bwTime;
}

Timestamp&
GpuLibrary::cursorFor(void* stream)
{
  if (stream == nullptr) return default_until_;
  return streams_[stream]; // idle (epoch 0) if unseen; max(now(),.) handles it
}

Timestamp
GpuLibrary::maxCursor() const
{
  Timestamp m = default_until_;
  for (const auto& kv : streams_) {
    if (kv.second > m) m = kv.second;
  }
  return m;
}

void
GpuLibrary::blockUntil(Timestamp t)
{
  Timestamp n = now();
  if (t > n) parent()->os()->blockTimeout(t - n);
}

void
GpuLibrary::enqueue(void* stream, double seconds)
{
  if (seconds < 0.0) seconds = 0.0;
  total_gpu_time_ += seconds;
  if (stream == nullptr) {
    // Default stream (legacy): a full barrier. Wait for all outstanding GPU
    // work, charge the cost synchronously on the host, then every cursor is
    // idle at the new now().
    blockUntil(maxCursor());
    if (seconds > 0.0) parent()->os()->blockTimeout(TimeDelta(seconds));
    Timestamp n = now();
    default_until_ = n;
    for (auto& kv : streams_) kv.second = n;
  } else {
    // Async: advance just this stream's cursor relative to now(); the host
    // thread is NOT blocked, so this work overlaps whatever the host does next.
    Timestamp& cur = cursorFor(stream);
    Timestamp start = now() > cur ? now() : cur;
    cur = start + TimeDelta(seconds);
  }
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
GpuLibrary::memcpy(void* /*dst*/, const void* /*src*/, uint64_t bytes,
                   int kind, void* stream)
{
  ++memcpy_count_;
  enqueue(stream, transferTime(bytes, kind));
}

void
GpuLibrary::launch(const char* /*kernelName*/,
                   uint32_t gx, uint32_t gy, uint32_t gz,
                   uint32_t bx, uint32_t by, uint32_t bz,
                   uint64_t /*shmemBytes*/, void* stream,
                   uint64_t flops, uint64_t intops,
                   uint64_t bytesRead, uint64_t bytesWritten)
{
  ++launch_count_;
  uint64_t totalThreads = static_cast<uint64_t>(gx) * gy * gz
                        * static_cast<uint64_t>(bx) * by * bz;
  enqueue(stream, kernelTime(totalThreads, flops, intops, bytesRead, bytesWritten));
}

void*
GpuLibrary::streamCreate()
{
  return reinterpret_cast<void*>(next_handle_++);
}

void
GpuLibrary::streamDestroy(void* s)
{
  streams_.erase(s);
}

void
GpuLibrary::streamSync(void* s)
{
  blockUntil(cursorFor(s));
}

void*
GpuLibrary::eventCreate()
{
  return reinterpret_cast<void*>(next_handle_++);
}

void
GpuLibrary::eventRecord(void* evt, void* stream)
{
  // The event fires when the stream reaches its current tail (or now() if the
  // stream is already idle). Default stream: when all outstanding work drains.
  Timestamp tail = (stream == nullptr) ? maxCursor() : cursorFor(stream);
  Timestamp n = now();
  events_[evt] = (tail > n) ? tail : n;
}

void
GpuLibrary::eventSync(void* evt)
{
  auto it = events_.find(evt);
  if (it != events_.end()) blockUntil(it->second);
}

float
GpuLibrary::eventElapsedMs(void* start, void* stop)
{
  auto a = events_.find(start);
  auto b = events_.find(stop);
  if (a == events_.end() || b == events_.end()) return 0.0f;
  if (b->second > a->second) {
    return static_cast<float>((b->second - a->second).msec());
  }
  return 0.0f;
}

void
GpuLibrary::deviceSync()
{
  blockUntil(maxCursor());
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
