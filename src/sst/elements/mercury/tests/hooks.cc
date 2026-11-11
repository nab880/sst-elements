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

// Calls the hooks that hgcc writes into skeletons, as rewritten code would.
// The first argument picks which ones (see hooks.py).

#define ssthg_app_name hooks
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mercury/common/skeleton.h>

// With HOOKS_STATIC_INIT set (hooks.py --mode static-init sets it), a static
// initializer calls a hook while the library loads, before any simulated
// thread runs, as a C++ global's initializer in a rewritten app can.
static int static_init_hook = getenv("HOOKS_STATIC_INIT")
    ? (sst_hg_compute_detailed(1, 0, 0), 1) : 0;

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "";
  if (!strcmp(mode, "free")) {
    sst_hg_free(malloc(64));
    sst_hg_free(nullptr);
    // Skeletons pass addresses in Mercury's null-buffer range instead of
    // real buffers; freeing one would crash. Try its start, middle and end.
    sst_hg_free(sst_hg_nullptr);
    sst_hg_free(sst_hg_nullptr_recv);
    sst_hg_free((char*)sst_hg_nullptr_range_max - 1);
    printf("free: done\n");
  } else if (!strcmp(mode, "compute")) {
    // At 2.1 GHz one operation takes 476 ps (the cycle rounded down to whole
    // picoseconds), so 2.1e6 flops take 0.9996 ms on one thread and 0.2499 ms
    // on four, and 0.2499 ms again when 64 threads are asked for on the
    // four-core node. 2.1e6 intops cost the same as the flops. 1.12 MB of
    // memory traffic is about 25 us; charged as operations it would be
    // 0.53 ms.
    sst_hg_compute_detailed(2100000, 0, 0);
    sst_hg_compute_detailed_nthr(2100000, 0, 0, 4);
    sst_hg_compute_detailed_nthr(2100000, 0, 0, 64);
    sst_hg_compute_detailed(0, 2100000, 0);
    sst_hg_compute_detailed(0, 0, 1120000);
    printf("compute: done\n");
  } else if (!strcmp(mode, "compute-zero")) {
    sst_hg_compute_detailed_nthr(2100000, 0, 0, 0);
  }
  return 0;
}
