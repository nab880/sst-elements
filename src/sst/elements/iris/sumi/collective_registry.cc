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

#include <iris/sumi/collective_registry.h>
#include <map>
#include <utility>

namespace SST::Iris::sumi {

namespace {
using RegistryKey = std::pair<Collective::type_t, std::string>;
using RegistryMap = std::map<RegistryKey, CollectiveFactory>;

// Function-local static: single table shared across every translation unit and
// across a dlopen'd plugin resolved RTLD_GLOBAL into libsumi.
RegistryMap& registry()
{
  static RegistryMap the_registry;
  return the_registry;
}
}

void
CollectiveRegistry::reg(Collective::type_t ty, const std::string& name,
                        CollectiveFactory factory)
{
  registry()[RegistryKey(ty, name)] = std::move(factory);
}

Collective*
CollectiveRegistry::make(Collective::type_t ty, const std::string& name,
                         const CollectiveFactoryArgs& args)
{
  if (name.empty()) return nullptr;
  auto it = registry().find(RegistryKey(ty, name));
  if (it == registry().end()) return nullptr;
  return it->second(args);
}

bool
CollectiveRegistry::has(Collective::type_t ty, const std::string& name)
{
  if (name.empty()) return false;
  return registry().count(RegistryKey(ty, name)) != 0;
}

}
