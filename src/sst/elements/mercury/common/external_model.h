// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// External model plugin interface for sst-hgcc.
// Supports C shared library plugins (dlopen) and Python subprocess plugins.

#ifndef mercury_common_external_model_h
#define mercury_common_external_model_h

#include <string>
#include <unordered_map>
#include <cstdio>

namespace SST {
namespace Hg {

struct ExternalModelEntry {
  enum Type { SHARED_LIB, PYTHON };
  Type type;

  // Shared library state
  void* dl_handle = nullptr;
  double (*predict_fn)(int, const double*) = nullptr;

  // Python subprocess state
  FILE* child_stdin  = nullptr;
  FILE* child_stdout = nullptr;
  pid_t child_pid    = -1;
};

class ExternalModelRegistry {
public:
  ~ExternalModelRegistry();

  double invoke(const std::string& name, int nargs, const double* args);

  void registerFromParams(const std::string& name,
                          const std::string& type,
                          const std::string& path);

private:
  bool loadSharedLib(ExternalModelEntry& entry, const std::string& path);
  bool spawnPython(ExternalModelEntry& entry, const std::string& script);
  void cleanup();

  std::unordered_map<std::string, ExternalModelEntry> models_;
};

} // namespace Hg
} // namespace SST

#endif
