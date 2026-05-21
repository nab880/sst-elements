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

#include <mercury/common/errors.h>
#include <mercury/operating_system/process/global.h>
#include <mercury/operating_system/process/thread.h>
#include <mercury/operating_system/process/cppglobal.h>

#include <cstdlib>

extern "C" {
char* static_init_glbls_segment = nullptr;
char* static_init_tls_segment = nullptr;
void allocate_static_init_tls_segment(){
  static_init_tls_segment = new char[int(1e6)];
}
void allocate_static_init_glbls_segment(){
  static_init_glbls_segment = new char[int(1e6)];
}

}

namespace SST {
namespace Hg {

GlobalVariableContext GlobalVariable::glblCtx;
GlobalVariableContext GlobalVariable::tlsCtx;
bool GlobalVariable::inited = false;

// One-time read of SST_HG_VALIDATE_GLOBALS at static-init time. Cached in a
// plain bool so the hot path in get_global_at_offset / get_tls_at_offset costs
// a single load + a predicted-not-taken branch when the flag is off.
// Accepted truthy values: any non-empty string whose first character is not '0'.
bool GlobalVariable::validateMaps = []{
  const char* v = std::getenv("SST_HG_VALIDATE_GLOBALS");
  return v && *v && v[0] != '0';
}();

void validate_global_map_or_abort(void* globalMap, void* stackPtr,
                                  intptr_t stackTopInt, bool isTls)
{
  // Slow path: only reached when SST_HG_VALIDATE_GLOBALS is on AND the
  // stack-pointer-aligned lookup pointed at memory that no GlobalVariableContext
  // ever registered as an active per-app segment. Almost always means a global
  // was accessed from the main simulator stack (or some other non-coroutine
  // stack) -- the same class of failure that ASLR exposes as a ~30% MPI_Init
  // flake on Linux.
  sst_hg_abort_printf(
    "%s access from non-coroutine stack: globalMap=%p (not a registered "
    "active segment); stackPtr=%p stackTopInt=0x%lx stacksize=%d. "
    "This typically means a global was touched before / outside a Mercury "
    "user-space thread switch. Disable SST_HG_VALIDATE_GLOBALS to silence.",
    isTls ? "TLS" : "global",
    globalMap, stackPtr,
    static_cast<unsigned long>(stackTopInt),
    sst_hg_global_stacksize);
}

int
GlobalVariable::init(const int size, const char* name, bool tls)
{
  if (!inited){
    tlsCtx.init();
    glblCtx.init();
    inited = true;
  }
  if (tls) return tlsCtx.append(size, name);
  else return glblCtx.append(size, name);
}

void
GlobalVariableContext::init()
{
  stackOffset = 0;
  allocSize_ = 4096;
  globalInits = nullptr;
}

void
GlobalVariableContext::registerInitFxn(int offset, std::function<void (void *)> &&fxn)
{
  initFxns[offset] = std::move(fxn);
}

int
GlobalVariableContext::append(const int size, const char*  /*name*/)
{
  int offset = stackOffset;

  int rem = size % 4;
  int offsetIncrement = rem ? (size + (4-rem)) : size; //align on 32-bits

  bool realloc = false;
  while ((offsetIncrement + stackOffset) > allocSize_){
    realloc = true;
    allocSize_ *= 2; //grow until big enough
  }

  if (realloc && !activeGlobalMaps_.empty()){
    sst_hg_abort_printf("dynamically loaded global variable overran storage space\n"
                      "to fix, explicitly set globals_size = %d in app params",
                      allocSize_);
  }

  if (realloc || globalInits == nullptr){
    char* old = globalInits;
    globalInits = new char[allocSize_];
    if (old){
      //move everything in that was already there
      ::memcpy(globalInits, old, stackOffset);
      delete[] old;
    }
  }

  //printf("Allocated global variable %s of size %d at offset %d - %s\n",
  //       name, size, offset, (realloc ? "reallocated to fit" : "already fits"));
  //fflush(stdout);

  stackOffset += offsetIncrement;

  return offset;
}

CppGlobalRegisterGuard::CppGlobalRegisterGuard(int& offset, int size, bool tls, const char* name,
                                               std::function<void(void*)>&& fxn) :
  tls_(tls), offset_(offset)
{
  offset_ = offset = GlobalVariable::init(size, name, tls);
  if (tls){
    GlobalVariable::tlsCtx.registerInitFxn(offset, std::move(fxn));
  } else {
    GlobalVariable::glblCtx.registerInitFxn(offset, std::move(fxn));
  }
}

CppGlobalRegisterGuard::~CppGlobalRegisterGuard()
{
  if (tls_){
    GlobalVariable::tlsCtx.unregisterInitFxn(offset_);
  } else {
    GlobalVariable::glblCtx.unregisterInitFxn(offset_);
  }
}

void
GlobalVariableContext::callInitFxns(void *globals)
{
  for (auto& pair : initFxns){
    int offset = pair.first;
    char* ptr = ((char*)globals) + offset;
    (pair.second)(ptr);
  }
}

GlobalVariableContext::~GlobalVariableContext()
{
  if (globalInits){
    delete[] globalInits;
    globalInits = nullptr;
  }
}

void
GlobalVariableContext::initGlobalSpace(void* ptr, int size, int offset)
{
  for (void* globals : activeGlobalMaps_){
    char* dst = ((char*)globals) + offset;
    ::memcpy(dst, ptr, size);
  }

  //also do the global init for any new threads spawned
  char* dst = ((char*)globalInits) + offset;
  ::memcpy(dst, ptr, size);
}

}
}
