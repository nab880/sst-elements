// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// C-linkage shims for ssthg_clang compute pragma output.
// These bridge #pragma sst compute / compute_explicit / compute model()
// calls to Mercury's ComputeLibrary at simulation time.

#include <mercury/operating_system/process/thread.h>
#include <mercury/operating_system/process/app.h>
#include <mercury/libraries/compute/compute_library.h>

#include <sst/core/params.h>
#include <cstdio>

static SST::Hg::ComputeLibrary*
getComputeLib()
{
  auto* thr = SST::Hg::Thread::current();
  if (!thr) return nullptr;
  auto* app = thr->parentApp();
  if (!app) return nullptr;
  auto* lib = app->getLibrary("ComputeLibrary");
  return dynamic_cast<SST::Hg::ComputeLibrary*>(lib);
}

extern "C" {

void sst_hg_compute_detailed(uint64_t flops, uint64_t intops, uint64_t bytes)
{
  auto* cl = getComputeLib();
  if (cl) cl->computeDetailed(flops, intops, bytes, 1);
}

void sst_hg_compute_detailed_nthr(uint64_t flops, uint64_t intops,
                                  uint64_t bytes, int nthread)
{
  auto* cl = getComputeLib();
  if (cl) cl->computeDetailed(flops, intops, bytes, nthread);
}

void sst_hg_compute_detailed_rw(uint64_t flops, uint64_t intops,
                                uint64_t readBytes, uint64_t writeBytes,
                                int nthread)
{
  auto* cl = getComputeLib();
  if (cl) cl->computeDetailed(flops, intops, readBytes + writeBytes, nthread);
}

void sst_hg_compute_model(const char* model_name)
{
  auto* thr = SST::Hg::Thread::current();
  if (!thr) return;
  auto* app = thr->parentApp();
  if (!app) return;
  auto* cl = dynamic_cast<SST::Hg::ComputeLibrary*>(
      app->getLibrary("ComputeLibrary"));
  if (!cl) return;

  SST::Params& params = app->params();

  std::string prefix = std::string("model.") + model_name + ".";
  uint64_t flops  = params.find<uint64_t>(prefix + "flops", 0);
  uint64_t intops = params.find<uint64_t>(prefix + "intops", 0);
  uint64_t bytes  = params.find<uint64_t>(prefix + "bytes", 0);
  int nthread     = params.find<int>(prefix + "nthread", 1);

  if (flops == 0 && intops == 0 && bytes == 0) {
    fprintf(stderr, "sst_hg_compute_model: warning: no params found for model '%s' "
            "(looked for %sflops, %sintops, %sbytes)\n",
            model_name, prefix.c_str(), prefix.c_str(), prefix.c_str());
    return;
  }

  cl->computeDetailed(flops, intops, bytes, nthread);
}

} // extern "C"
