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

#include <cstdint>

namespace SST {
namespace Hg {

/**
 * Abstract interface implemented by the GPU library (GpuLibrary) and called
 * by the extern "C" sst_hg_cuda_* bridge (cuda_runtime_api.cc). The bridge
 * lives in libhg and reaches the concrete library only through this base --
 * exactly the ComputeAPI / ComputeLibrary split (compute_api.h) -- so libhg
 * never links the loadable module. One method per sst_hg_cuda ABI verb
 * (hg_cuda.h); all calls run on the active Mercury OS thread.
 */
class GpuComputeAPI
{
 public:
  virtual ~GpuComputeAPI() {}

  virtual void* malloc(uint64_t bytes) = 0;
  virtual void free(void* dptr) = 0;
  virtual int isDevicePtr(const void* p) = 0;
  virtual void memcpy(void* dst, const void* src, uint64_t bytes,
                      int kind, void* stream) = 0;
  virtual void launch(const char* kernelName,
                      uint32_t gx, uint32_t gy, uint32_t gz,
                      uint32_t bx, uint32_t by, uint32_t bz,
                      uint64_t shmemBytes, void* stream,
                      uint64_t flops, uint64_t intops,
                      uint64_t bytesRead, uint64_t bytesWritten) = 0;
  virtual void* streamCreate() = 0;
  virtual void streamDestroy(void* s) = 0;
  virtual void streamSync(void* s) = 0;
  virtual void* eventCreate() = 0;
  virtual void eventRecord(void* evt, void* stream) = 0;
  virtual void eventSync(void* evt) = 0;
  virtual float eventElapsedMs(void* start, void* stop) = 0;
  virtual void deviceSync() = 0;
  virtual int getDeviceCount() = 0;
  virtual void setDevice(int dev) = 0;
};

} // end namespace Hg
} // end namespace SST
