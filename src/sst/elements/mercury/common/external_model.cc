// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// External model plugin interface implementation.

#include <mercury/common/external_model.h>
#include <mercury/operating_system/process/thread.h>
#include <mercury/operating_system/process/app.h>
#include <mercury/components/operating_system_api.h>
#include <mercury/common/timestamp.h>
#include <sst/core/params.h>

#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace SST {
namespace Hg {

ExternalModelRegistry::~ExternalModelRegistry()
{
  cleanup();
}

void ExternalModelRegistry::cleanup()
{
  for (auto& kv : models_) {
    auto& e = kv.second;
    if (e.type == ExternalModelEntry::SHARED_LIB && e.dl_handle) {
      dlclose(e.dl_handle);
      e.dl_handle = nullptr;
    }
    if (e.type == ExternalModelEntry::PYTHON) {
      if (e.child_stdin) {
        fclose(e.child_stdin);
        e.child_stdin = nullptr;
      }
      if (e.child_stdout) {
        fclose(e.child_stdout);
        e.child_stdout = nullptr;
      }
      if (e.child_pid > 0) {
        kill(e.child_pid, SIGTERM);
        e.child_pid = -1;
      }
    }
  }
  models_.clear();
}

void ExternalModelRegistry::registerFromParams(
    const std::string& name,
    const std::string& type,
    const std::string& path)
{
  if (models_.find(name) != models_.end()) return;

  ExternalModelEntry entry;
  if (type == "shared_lib") {
    entry.type = ExternalModelEntry::SHARED_LIB;
    if (!loadSharedLib(entry, path)) {
      fprintf(stderr, "sst_hg_external_model: failed to load shared lib '%s' for model '%s'\n",
              path.c_str(), name.c_str());
      return;
    }
  } else if (type == "python") {
    entry.type = ExternalModelEntry::PYTHON;
    if (!spawnPython(entry, path)) {
      fprintf(stderr, "sst_hg_external_model: failed to spawn python for model '%s' script '%s'\n",
              name.c_str(), path.c_str());
      return;
    }
  } else {
    fprintf(stderr, "sst_hg_external_model: unknown type '%s' for model '%s' "
            "(expected 'shared_lib' or 'python')\n", type.c_str(), name.c_str());
    return;
  }
  models_[name] = entry;
}

bool ExternalModelRegistry::loadSharedLib(ExternalModelEntry& entry,
                                          const std::string& path)
{
  entry.dl_handle = dlopen(path.c_str(), RTLD_LAZY);
  if (!entry.dl_handle) {
    fprintf(stderr, "dlopen error: %s\n", dlerror());
    return false;
  }
  dlerror(); // clear
  void* sym = dlsym(entry.dl_handle, "sst_model_predict");
  const char* err = dlerror();
  if (err) {
    fprintf(stderr, "dlsym error: %s\n", err);
    dlclose(entry.dl_handle);
    entry.dl_handle = nullptr;
    return false;
  }
  entry.predict_fn = reinterpret_cast<double(*)(int, const double*)>(sym);
  return true;
}

bool ExternalModelRegistry::spawnPython(ExternalModelEntry& entry,
                                        const std::string& script)
{
  int pipe_in[2];   // parent writes -> child reads (child stdin)
  int pipe_out[2];  // child writes -> parent reads (child stdout)

  if (pipe(pipe_in) != 0 || pipe(pipe_out) != 0) {
    perror("pipe");
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return false;
  }

  if (pid == 0) {
    // Child process
    close(pipe_in[1]);   // close parent's write end
    close(pipe_out[0]);  // close parent's read end
    dup2(pipe_in[0], STDIN_FILENO);
    dup2(pipe_out[1], STDOUT_FILENO);
    close(pipe_in[0]);
    close(pipe_out[1]);
    execlp("python3", "python3", script.c_str(), (char*)nullptr);
    perror("execlp python3");
    _exit(1);
  }

  // Parent process
  close(pipe_in[0]);
  close(pipe_out[1]);
  entry.child_stdin  = fdopen(pipe_in[1], "w");
  entry.child_stdout = fdopen(pipe_out[0], "r");
  entry.child_pid    = pid;
  return (entry.child_stdin && entry.child_stdout);
}

double ExternalModelRegistry::invoke(const std::string& name,
                                     int nargs, const double* args)
{
  auto it = models_.find(name);
  if (it == models_.end()) return 0.0;

  auto& entry = it->second;
  if (entry.type == ExternalModelEntry::SHARED_LIB) {
    if (!entry.predict_fn) return 0.0;
    return entry.predict_fn(nargs, args);
  }

  // Python subprocess
  if (!entry.child_stdin || !entry.child_stdout) return 0.0;

  std::ostringstream line;
  for (int i = 0; i < nargs; ++i) {
    if (i > 0) line << ' ';
    line << args[i];
  }
  line << '\n';
  std::string msg = line.str();

  if (fputs(msg.c_str(), entry.child_stdin) == EOF) return 0.0;
  fflush(entry.child_stdin);

  char buf[256];
  if (!fgets(buf, sizeof(buf), entry.child_stdout)) return 0.0;
  return strtod(buf, nullptr);
}

} // namespace Hg
} // namespace SST

// ---- C-linkage shim called by ssthg_clang-generated code ----

static SST::Hg::ExternalModelRegistry*
getOrCreateRegistry()
{
  auto* thr = SST::Hg::Thread::current();
  if (!thr) return nullptr;
  auto* app = thr->parentApp();
  if (!app) return nullptr;

  // One registry per app, stored as a thread-local-ish pointer keyed in a
  // static map.  Because SST runs cooperatively, no real mutex is needed.
  static std::unordered_map<SST::Hg::App*, SST::Hg::ExternalModelRegistry*> registries;
  auto& reg = registries[app];
  if (!reg) {
    reg = new SST::Hg::ExternalModelRegistry();
  }
  return reg;
}

static void ensureModelLoaded(SST::Hg::ExternalModelRegistry* reg,
                              const std::string& name)
{
  auto* thr = SST::Hg::Thread::current();
  if (!thr) return;
  auto* app = thr->parentApp();
  if (!app) return;

  SST::Params& params = app->params();
  std::string prefix = "external_model." + name + ".";
  std::string type = params.find<std::string>(prefix + "type", "");
  if (type.empty()) {
    fprintf(stderr, "sst_hg_external_model_invoke: no params found for model '%s' "
            "(looked for %stype)\n", name.c_str(), prefix.c_str());
    return;
  }
  std::string path;
  if (type == "shared_lib") {
    path = params.find<std::string>(prefix + "path", "");
  } else if (type == "python") {
    path = params.find<std::string>(prefix + "script", "");
  }
  reg->registerFromParams(name, type, path);
}

extern "C" {

void sst_hg_external_model_invoke(const char* model_name,
                                  int nargs, const double* args)
{
  auto* reg = getOrCreateRegistry();
  if (!reg) return;

  std::string name(model_name);
  ensureModelLoaded(reg, name);

  double ns = reg->invoke(name, nargs, args);
  if (ns <= 0.0) return;

  auto* thr = SST::Hg::Thread::current();
  if (!thr) return;
  auto* app = thr->parentApp();
  if (!app) return;

  SST::Hg::TimeDelta delay(ns * 1e-9);
  app->os()->blockTimeout(delay);
}

} // extern "C"
