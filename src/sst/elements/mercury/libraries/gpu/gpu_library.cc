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
#include <external/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
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
      table_loaded_(false),
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

  // Wave / tile quantization (WS1-1b). sm_count_ == 0 disables it, so the model
  // is the smooth roofline unless a device SM count is given. The occupancy cap
  // bounds resident blocks per SM by both the thread budget and a hard block cap.
  sm_count_ = params.find<uint64_t>("gpu_sm_count", 0);
  max_threads_per_sm_ = params.find<uint64_t>("gpu_max_threads_per_sm", 2048);
  max_blocks_per_sm_ = params.find<uint64_t>("gpu_max_blocks_per_sm", 32);

  // Optional measured calibration table; absent -> pure roofline.
  std::string calFile = params.find<std::string>("gpu_kernel_times", "");
  if (!calFile.empty()) loadCalibration(calFile);
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

uint64_t
GpuLibrary::blocksPerSm(uint64_t threadsPerBlock) const
{
  if (threadsPerBlock == 0) return 1;
  uint64_t byThreads = max_threads_per_sm_ / threadsPerBlock; // occupancy cap
  uint64_t bps = std::min<uint64_t>(byThreads, max_blocks_per_sm_);
  return bps < 1 ? 1 : bps;
}

double
GpuLibrary::kernelTime(uint64_t blocks, uint64_t threadsPerBlock,
                       uint64_t flopsPerThread, uint64_t intopsPerThread,
                       uint64_t bytesReadPerThread,
                       uint64_t bytesWrittenPerThread) const
{
  double threads =
      static_cast<double>(blocks) * static_cast<double>(threadsPerBlock);
  double flopsTotal =
      static_cast<double>(flopsPerThread + intopsPerThread) * threads;
  double bytesTotal =
      static_cast<double>(bytesReadPerThread + bytesWrittenPerThread) * threads;
  double computeT = peak_flops_ > 0.0 ? flopsTotal / peak_flops_ : 0.0;
  double memT = mem_bandwidth_ > 0.0 ? bytesTotal / mem_bandwidth_ : 0.0;
  double smooth = std::max(computeT, memT);

  // Wave / tile quantization (WS1-1b): thread blocks run in waves across the
  // SMs -- concurrent = sm_count * blocks_per_sm run at once -- so the wall time
  // is ceil(blocks/concurrent) waves even when the last wave is partial. The
  // penalty is the actual wave count over the ideal fractional waves: >= 1, and
  // == 1 exactly when the blocks tile the machine evenly. A kernel too small to
  // fill the GPU (blocks < concurrent) is penalized for underutilization -- the
  // staircase a smooth roofline cannot show. sm_count_ == 0 leaves penalty == 1.
  double penalty = 1.0;
  if (sm_count_ > 0 && blocks > 0 && threadsPerBlock > 0) {
    double concurrent = static_cast<double>(sm_count_)
                      * static_cast<double>(blocksPerSm(threadsPerBlock));
    double idealWaves = static_cast<double>(blocks) / concurrent;
    if (idealWaves > 0.0) penalty = std::ceil(idealWaves) / idealWaves;
  }
  return launch_overhead_ + smooth * penalty;
}

void
GpuLibrary::loadCalibration(const std::string& path)
{
  std::ifstream f(path);
  if (!f.good()) {
    sst_hg_abort_printf("gpu: cannot open gpu_kernel_times file '%s'", path.c_str());
  }
  nlohmann::json j;
  try {
    f >> j;
  } catch (const std::exception& e) {
    sst_hg_abort_printf("gpu: failed to parse gpu_kernel_times '%s': %s",
                        path.c_str(), e.what());
  }
  int version = j.value("version", 0);
  if (version != 1) {
    sst_hg_abort_printf("gpu: gpu_kernel_times '%s' has unsupported version %d "
                        "(expected 1)", path.c_str(), version);
  }
  auto kernelsIt = j.find("kernels");
  if (kernelsIt != j.end()) {
    for (auto& kv : kernelsIt->items()) {
      std::vector<std::pair<double, double>> pts;
      for (auto& s : kv.value()) {
        double threads = s.at("threads").get<double>();
        double seconds = s.at("seconds").get<double>();
        if (threads <= 0.0 || seconds <= 0.0) {
          sst_hg_abort_printf("gpu: gpu_kernel_times '%s' kernel '%s' has a "
                              "non-positive threads/seconds sample",
                              path.c_str(), kv.key().c_str());
        }
        pts.emplace_back(threads, seconds);
      }
      if (!pts.empty()) {
        std::sort(pts.begin(), pts.end());
        table_[kv.key()] = std::move(pts);
      }
    }
  }
  table_loaded_ = true;
}

double
GpuLibrary::kernelTimeFromTable(const std::string& name, uint64_t totalThreads,
                                bool& found) const
{
  found = false;
  auto it = table_.find(name);
  if (it == table_.end()) return 0.0;
  const auto& pts = it->second; // non-empty, sorted by thread count, all > 0
  found = true;

  double x = static_cast<double>(totalThreads);
  if (x <= pts.front().first) return pts.front().second;  // clamp below
  if (x >= pts.back().first) return pts.back().second;    // clamp above
  for (size_t i = 1; i < pts.size(); ++i) {
    if (x <= pts[i].first) {
      double x0 = pts[i - 1].first, y0 = pts[i - 1].second;
      double x1 = pts[i].first,     y1 = pts[i].second;
      // Log-log interpolation: linear in (log threads, log seconds).
      double frac = (std::log(x) - std::log(x0)) / (std::log(x1) - std::log(x0));
      return std::exp(std::log(y0) + frac * (std::log(y1) - std::log(y0)));
    }
  }
  return pts.back().second; // unreachable (loop covers the open interval)
}

void
GpuLibrary::warnTableMiss(const std::string& name)
{
  if (warned_.insert(name).second) {
    parent()->cerrStream()
        << "[gpu] warning: kernel '" << name << "' is not in the "
        << "gpu_kernel_times table; using the roofline model" << std::endl;
  }
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
GpuLibrary::launch(const char* kernelName,
                   uint32_t gx, uint32_t gy, uint32_t gz,
                   uint32_t bx, uint32_t by, uint32_t bz,
                   uint64_t /*shmemBytes*/, void* stream,
                   uint64_t flops, uint64_t intops,
                   uint64_t bytesRead, uint64_t bytesWritten)
{
  ++launch_count_;
  uint64_t blocks = static_cast<uint64_t>(gx) * gy * gz;
  uint64_t threadsPerBlock = static_cast<uint64_t>(bx) * by * bz;
  uint64_t totalThreads = blocks * threadsPerBlock;

  // Calibration table first (the accurate, measured path); roofline otherwise.
  std::string name = kernelName ? kernelName : "";
  bool fromTable = false;
  double t = kernelTimeFromTable(name, totalThreads, fromTable);
  if (!fromTable) {
    if (table_loaded_) warnTableMiss(name);
    t = kernelTime(blocks, threadsPerBlock, flops, intops, bytesRead, bytesWritten);
  }
  enqueue(stream, t);
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
