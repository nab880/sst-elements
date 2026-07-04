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

// sst_hg_cuda_* ABI bridge in libhg; forwards to per-rank GpuLibrary (like pthread/MPI).

#include <mercury/libraries/gpu/hg_cuda.h>
#include <mercury/libraries/gpu/gpu_api.h>
#include <mercury/operating_system/process/thread.h>

namespace {

SST::Hg::GpuComputeAPI* gpu()
{
  // Aborts if the app did not declare "gpulibrary:GpuLibrary" in its libraries list.
  return SST::Hg::Thread::current()->getLibrary<SST::Hg::GpuComputeAPI>("GpuLibrary");
}

} // namespace

extern "C" {

void* sst_hg_cuda_malloc(uint64_t bytes)
{
  return gpu()->malloc(bytes);
}

void sst_hg_cuda_free(void* dptr)
{
  gpu()->free(dptr);
}

int sst_hg_cuda_is_device_ptr(const void* p)
{
  return gpu()->isDevicePtr(p);
}

void sst_hg_cuda_memcpy(void* dst, const void* src, uint64_t bytes, int kind,
                        void* stream)
{
  gpu()->memcpy(dst, src, bytes, kind, stream);
}

void sst_hg_cuda_launch(const char* kernelName,
                        uint32_t gx, uint32_t gy, uint32_t gz,
                        uint32_t bx, uint32_t by, uint32_t bz,
                        uint64_t shmemBytes, void* stream,
                        uint64_t flops, uint64_t intops,
                        uint64_t bytesRead, uint64_t bytesWritten)
{
  gpu()->launch(kernelName, gx, gy, gz, bx, by, bz, shmemBytes, stream,
                flops, intops, bytesRead, bytesWritten);
}

void* sst_hg_cuda_stream_create(void)
{
  return gpu()->streamCreate();
}

void sst_hg_cuda_stream_destroy(void* s)
{
  gpu()->streamDestroy(s);
}

void sst_hg_cuda_stream_sync(void* s)
{
  gpu()->streamSync(s);
}

void* sst_hg_cuda_event_create(void)
{
  return gpu()->eventCreate();
}

void sst_hg_cuda_event_record(void* evt, void* stream)
{
  gpu()->eventRecord(evt, stream);
}

void sst_hg_cuda_event_sync(void* evt)
{
  gpu()->eventSync(evt);
}

float sst_hg_cuda_event_elapsed_ms(void* start, void* stop)
{
  return gpu()->eventElapsedMs(start, stop);
}

void sst_hg_cuda_device_sync(void)
{
  gpu()->deviceSync();
}

int sst_hg_cuda_get_device_count(void)
{
  return gpu()->getDeviceCount();
}

void sst_hg_cuda_set_device(int dev)
{
  gpu()->setDevice(dev);
}

} // extern "C"
