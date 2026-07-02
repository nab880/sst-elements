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

#include <iris/sumi/collective.h>
#include <iris/sumi/comm_functions.h>
#include <iris/sumi/transport_fwd.h>
#include <iris/sumi/communicator_fwd.h>
#include <functional>
#include <string>

namespace SST::Iris::sumi {

// The superset of arguments the scalar-count CollectiveEngine::<op> methods
// thread through to a Collective constructor. A factory picks the fields its
// constructor needs (allreduce ignores root, allgather ignores fxn, ...); the
// v-variants take per-rank int* count arrays and are intentionally not covered.
struct CollectiveFactoryArgs {
  CollectiveEngine* engine;
  void* dst;
  void* src;
  int root;
  int nelems;
  int type_size;
  int tag;
  int cq_id;
  reduce_fxn fxn;
  Communicator* comm;
};

using CollectiveFactory =
  std::function<Collective*(const CollectiveFactoryArgs&)>;

// Name-keyed factory table, one namespace per Collective::type_t. Lets a user
// drop in a DAG-based algorithm and select it by name instead of editing the
// hard-coded dispatch in sim_transport.cc. Storage is a function-local static
// singleton in libsumi so registrations from a dlopen'd plugin (loaded
// RTLD_GLOBAL) land in the same table the engine reads (see T3).
class CollectiveRegistry {
 public:
  static void reg(Collective::type_t ty, const std::string& name,
                  CollectiveFactory factory);

  // nullptr if (ty, name) is unregistered or name is empty.
  static Collective* make(Collective::type_t ty, const std::string& name,
                          const CollectiveFactoryArgs& args);

  static bool has(Collective::type_t ty, const std::string& name);
};

// Registers CLASS under (TYPE, NAME) at static-init. The trailing args are the
// constructor call spelled against the `a` (CollectiveFactoryArgs) parameter,
// so any constructor shape fits: allreduce-style (a.src + a.fxn, no root),
// bcast-style (a.root, no src/fxn), and so on.
#define SUMI_REGISTER_COLLECTIVE(TYPE, NAME, CLASS, ...)                       \
  static bool _sumi_reg_##CLASS = [](){                                        \
    ::SST::Iris::sumi::CollectiveRegistry::reg(                                \
      ::SST::Iris::sumi::Collective::TYPE, NAME,                               \
      [](const ::SST::Iris::sumi::CollectiveFactoryArgs& a)                    \
          -> ::SST::Iris::sumi::Collective* {                                  \
        return new CLASS(__VA_ARGS__);                                         \
      });                                                                      \
    return true;                                                               \
  }();

}
