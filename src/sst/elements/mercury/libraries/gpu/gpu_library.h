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

#include <mercury/operating_system/libraries/library.h>
#include <mercury/common/timestamp.h>
#include <mercury/libraries/gpu/gpu_api.h>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace SST {
namespace Hg {

/**
 * Roofline implementation of the sst_hg_cuda ABI (Phase 3 Track A). Kernels
 * never execute: a launch is charged a modeled time derived from the grid/block
 * dimensions and the per-thread costs (roofline -- compute vs. memory bound), a
 * memcpy is charged a PCIe (H2D/D2H) or HBM (D2D) transfer time. Each stream is
 * an independent "busy-until" timeline: async ops advance the stream cursor
 * without blocking the host thread (so kernels/copies overlap host compute and
 * MPI), and only sync ops block the host until a cursor. The default stream
 * (handle 0) is a legacy barrier. A measured calibration table (the
 * gpu_kernel_times JSON) overrides the roofline per kernel when present. One
 * instance per rank (per App). Loaded on demand when an app declares
 * "gpulibrary:GpuLibrary" in its libraries list, like ComputeLibrary.
 */
class GpuLibrary : public GpuComputeAPI, public Library
{
 public:
  SST_ELI_REGISTER_DERIVED(
    Library,
    GpuLibrary,
    "gpulibrary",
    "GpuLibrary",
    SST_ELI_ELEMENT_VERSION(1,0,0),
    "models GPU kernel and transfer time with a roofline model")

  GpuLibrary(SST::Params& params, App* parent);

  ~GpuLibrary() override;

  void* malloc(uint64_t bytes) override;
  void free(void* dptr) override;
  int isDevicePtr(const void* p) override;
  void memcpy(void* dst, const void* src, uint64_t bytes,
              int kind, void* stream) override;
  void launch(const char* kernelName,
              uint32_t gx, uint32_t gy, uint32_t gz,
              uint32_t bx, uint32_t by, uint32_t bz,
              uint64_t shmemBytes, void* stream,
              uint64_t flops, uint64_t intops,
              uint64_t bytesRead, uint64_t bytesWritten) override;
  void* streamCreate() override;
  void streamDestroy(void* s) override;
  void streamSync(void* s) override;
  void* eventCreate() override;
  void eventRecord(void* evt, void* stream) override;
  void eventSync(void* evt) override;
  float eventElapsedMs(void* start, void* stop) override;
  void deviceSync() override;
  int getDeviceCount() override;
  void setDevice(int dev) override;

 private:
  // Enqueue `seconds` of work on `stream` (0 = default). A non-default stream
  // advances its cursor relative to now() without blocking the host thread (so
  // it overlaps); the default stream is a legacy barrier that waits for all
  // streams, charges on the host, then resets every cursor to now().
  void enqueue(void* stream, double seconds);
  // Block the host thread until simulated time reaches `t` (no-op if past).
  void blockUntil(Timestamp t);
  // The latest cursor over the default and all explicit streams.
  Timestamp maxCursor() const;
  // The busy-until cursor for a stream handle (default stream for 0).
  Timestamp& cursorFor(void* stream);

  // Roofline kernel time (seconds): launch_overhead + max(compute, memory) over
  // the total thread count (blocks * threadsPerBlock), then scaled by the wave-
  // quantization penalty (WS1-1b). The intops term is folded into flops for now
  // (the rewriter reports both; a separate int-rate is a later refinement).
  double kernelTime(uint64_t blocks, uint64_t threadsPerBlock,
                    uint64_t flopsPerThread, uint64_t intopsPerThread,
                    uint64_t bytesReadPerThread,
                    uint64_t bytesWrittenPerThread) const;

  // Resident thread blocks per SM for occupancy: the thread budget
  // (max_threads_per_sm / threadsPerBlock) capped by the hard block limit, >= 1.
  uint64_t blocksPerSm(uint64_t threadsPerBlock) const;

  // Calibration (CUDA_PLAN.md §4.1). Load the gpu_kernel_times JSON; look a
  // kernel up by mangled name and total thread count (log-log interpolation
  // over the samples, clamped at the ends). `found` is false when the kernel
  // is absent, so the caller falls back to the roofline.
  void loadCalibration(const std::string& path);
  double kernelTimeFromTable(const std::string& name, uint64_t totalThreads,
                             bool& found) const;
  void warnTableMiss(const std::string& name);

  // Transfer time (seconds): D2D uses HBM bandwidth; H2D/D2H/Default/H2H use
  // the PCIe latency + bandwidth model.
  double transferTime(uint64_t bytes, int kind) const;

  // Roofline params (SI base units: flop/s, byte/s, seconds), read from params.
  double peak_flops_;
  double mem_bandwidth_;
  double pcie_latency_;
  double pcie_bandwidth_;
  double launch_overhead_;

  // Wave-quantization params (WS1-1b). sm_count_ == 0 disables the penalty.
  uint64_t sm_count_;
  uint64_t max_threads_per_sm_;
  uint64_t max_blocks_per_sm_;

  // Calibration table: mangled kernel name -> samples sorted by thread count.
  // Empty (and table_loaded_ false) unless gpu_kernel_times names a file.
  std::map<std::string, std::vector<std::pair<double, double>>> table_;
  bool table_loaded_;
  std::set<std::string> warned_; // kernels already warned as table-missing

  // Cumulative modeled GPU work on this rank (seconds; sums across streams, so
  // with overlap it exceeds wall time -- it is a busy-time total, not latency).
  double total_gpu_time_;

  // Per-stream "busy-until" timelines (the §9 design): the default stream and a
  // cursor per explicit handle; events capture a stream cursor at record time.
  Timestamp default_until_;
  std::map<void*, Timestamp> streams_;
  std::map<void*, Timestamp> events_;

  // Per-rank device-pointer cookie bump-allocator (CUDA_PLAN.md D6): high
  // canonical-invalid-ish base so an accidental host dereference faults loudly.
  uint64_t cookie_next_;
  uint64_t cookie_end_;

  // Trivial id space shared by streams and events (handles are never
  // dereferenced by the model; they only need to be distinct and non-null).
  uint64_t next_handle_;

  // Accounting (reported in the destructor; see the .cc).
  uint64_t launch_count_;
  uint64_t memcpy_count_;
};

} // end namespace Hg
} // end namespace SST
