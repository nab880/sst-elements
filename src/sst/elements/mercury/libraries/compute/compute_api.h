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

#include <mercury/common/errors.h>
#include <cstdint>

namespace SST {
namespace Hg {

class ComputeAPI
{
 public:
  virtual void sleep(TimeDelta time) = 0;
  virtual void compute(TimeDelta time) = 0;
  virtual void computeBlockMemcpy(uint64_t bytes) = 0;
  /** Charge a modeled instruction mix, as emitted by hgcc's
   *  `#pragma sst compute`. The default refuses rather than silently
   *  costing nothing, so an implementation that does not model this
   *  fails loudly at the first skeletonized loop. */
  virtual void computeDetailed(uint64_t flops, uint64_t intops,
                               uint64_t bytes, int nthread) {
    sst_hg_abort_printf("ComputeAPI implementation does not model detailed "
                        "compute; `#pragma sst compute` cannot be honored "
                        "(flops=%llu intops=%llu bytes=%llu nthread=%d)",
                        (unsigned long long)flops, (unsigned long long)intops,
                        (unsigned long long)bytes, nthread);
  }
  virtual void write(uint64_t bytes) = 0;
  virtual void copy(uint64_t bytes) = 0;
};

} // end namespace Hg
} // end namespace SST
