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

// Per-Mercury-thread TLS context, addressed via a single pthread-thread-local
// pointer. Replaces the legacy "SP-align-then-read" trick that conflated
// process-thread layout with per-coroutine state and was sensitive to Linux
// ASLR. Installed/cleared by the Mercury OS layer around every user-space
// thread context switch.

#ifdef __cplusplus
#include <cstdint>
extern "C" {
#else
#include <stdint.h>
#endif

extern int sst_hg_global_stacksize;

struct SstHgThreadTls {
  int   thread_id;
  void* global_map;      // per-app globals segment
  void* tls_map;         // per-thread TLS segment
  void* implicit_state;  // OS-implicit state slot
  int   sanity_check;    // populated with tls_sanity_check; debug aid
};

extern __thread struct SstHgThreadTls* sst_hg_current_tls;

#ifdef __cplusplus
} // extern "C"
#endif

#ifndef SST_HG_INLINE
#ifdef __STRICT_ANSI__
#define SST_HG_INLINE
#else
#define SST_HG_INLINE inline
#endif
#endif
