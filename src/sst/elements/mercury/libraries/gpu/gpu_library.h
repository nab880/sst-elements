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
#include <mercury/libraries/gpu/gpu_api.h>
#include <cstdint>

namespace SST {
namespace Hg {

/**
 * Fixed-cost stub implementation of the sst_hg_cuda ABI (Phase 2 Track 3).
 * Kernels never execute: a launch charges a fixed modeled time, a memcpy
 * charges a fixed time, syncs are no-ops, allocations hand out device-pointer
 * cookies from a reserved range, and streams/events are trivial id tables.
 * One instance per rank (per App). The real calibration/roofline timing model
 * is Phase 3; automatic per-thread costs are Phase 4. Loaded on demand when an
 * app declares "gpulibrary:GpuLibrary" in its libraries list, exactly like
 * ComputeLibrary.
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
    "models GPU kernel and memcpy time as a fixed-cost stub")

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
  // Per-rank device-pointer cookie bump-allocator (CUDA_PLAN.md D6): high
  // canonical-invalid-ish base so an accidental host dereference faults loudly.
  uint64_t cookie_next_;
  uint64_t cookie_end_;

  // Trivial id space shared by streams and events (handles are never
  // dereferenced by the stub; they only need to be distinct and non-null).
  uint64_t next_handle_;

  // Accounting (reported in the destructor; see the .cc).
  uint64_t launch_count_;
  uint64_t memcpy_count_;
};

} // end namespace Hg
} // end namespace SST
