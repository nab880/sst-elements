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
  }
  return 0;
}
