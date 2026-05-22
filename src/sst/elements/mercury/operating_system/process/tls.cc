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

#include <mercury/operating_system/process/tls.h>

// Single per-pthread storage slot. Set by the Mercury OS layer immediately
// before resuming a user-space thread and cleared on return. While unset,
// accessor inlines fall back to the static-init segments.
__thread struct SstHgThreadTls* sst_hg_current_tls = 0;
