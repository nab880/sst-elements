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

#include <mercury/operating_system/process/tls.h>
#include <mercury/common/unusedvariablemacro.h>

#ifndef SST_HG_INLINE
#ifdef __STRICT_ANSI__
#define SST_HG_INLINE
#else
#define SST_HG_INLINE inline
#endif
#endif


#ifdef __cplusplus
extern "C" {
#endif
extern char* static_init_glbls_segment;
extern char* static_init_tls_segment;
void allocate_static_init_glbls_segment();
void allocate_static_init_tls_segment();
#ifdef __cplusplus
}
#endif

// Per-coroutine TLS lookups now read through the pthread-thread-local pointer
// sst_hg_current_tls (installed by the Mercury OS layer around each context
// switch). When no Mercury thread is active -- e.g. during early static init
// before any App runs -- we fall back to the global "static init" segments,
// matching the previous behavior of the SP-trick accessors.

SST_HG_MAYBE_UNUSED
static SST_HG_INLINE char* get_sst_hg_global_data(){
  struct SstHgThreadTls* t = sst_hg_current_tls;
  if (!t) {
    if (!static_init_glbls_segment) allocate_static_init_glbls_segment();
    return static_init_glbls_segment;
  }
  return (char*) t->global_map;
}

SST_HG_MAYBE_UNUSED
static SST_HG_INLINE char* get_sst_hg_tls_data(){
  struct SstHgThreadTls* t = sst_hg_current_tls;
  if (!t) {
    if (!static_init_tls_segment) allocate_static_init_tls_segment();
    return static_init_tls_segment;
  }
  return (char*) t->tls_map;
}

SST_HG_MAYBE_UNUSED
static SST_HG_INLINE int get_sst_hg_tls_thread_id(){
  struct SstHgThreadTls* t = sst_hg_current_tls;
  return t ? t->thread_id : 0;
}

#undef SST_HG_INLINE
