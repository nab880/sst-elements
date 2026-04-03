#include "pmi.h"
#include <mercury/common/errors.h>
#include <mercury/common/hg_printf.h>
#include <mercury/common/thread_lock.h>
#include <mercury/components/operating_system.h>
#include <mercury/operating_system/process/app.h>
#include <mercury/operating_system/process/thread.h>
#include <iris/sumi/message.h>
#include <iris/sumi/transport.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#define debug(tport, ...) do { (void)(tport); } while(0)

static SST::Iris::sumi::Transport* sstmac_pmi()
{
  SST::Hg::Thread* t = SST::Hg::OperatingSystem::currentThread();
  return t->getLibrary<SST::Iris::sumi::Transport>("libfabric");
}

extern "C" int PMI_Get_rank(int *ret)
{
  *ret = sstmac_pmi()->rank();
  return PMI_SUCCESS;
}

extern "C" int PMI_Get_size(int* ret)
{
  *ret = sstmac_pmi()->nproc();
  return PMI_SUCCESS;
}

// Right now all simulated ranks share the same state, may need to be per-app
// in more complex runs
static SST::Hg::thread_lock kvs_lock;
static std::unordered_map<std::string,
            std::unordered_map<std::string, std::string>> kvs;

extern "C" int
PMI_KVS_Put(const char kvsname[], const char key[], const char value[])
{
  kvs_lock.lock();
  kvs[kvsname][key] = value;
  kvs_lock.unlock();
  return PMI_SUCCESS;
}

extern "C" int
PMI_KVS_Get( const char kvsname[], const char key[], char value[], int length)
{
  kvs_lock.lock();
  auto& str = kvs[kvsname][key];
  if (length < (int)(str.length() + 1)){
    kvs_lock.unlock();
    return PMI_ERR_INVALID_LENGTH;
  } else {
    ::strcpy(value, str.c_str());
    kvs_lock.unlock();
    return PMI_SUCCESS;
  }
}

extern "C" int
PMI2_KVS_Put(const char key[], const char value[])
{
  SST::Hg::abort("unimplemented error: PMI2_KVS_Put");
  return PMI_SUCCESS;
}

// TODO: This should look up `key` in the KVS (scoped by jobid/src_pmi_id),
// copy the value into `value` (up to maxvalue bytes), and set *vallen to
// the actual length.  Right now it is a no-op that returns SUCCESS, which
// will silently return uninitialized data to the caller.
extern "C" int
PMI2_KVS_Get(const char *jobid, int src_pmi_id, const char key[], char value [], int maxvalue, int *vallen)
{
  return PMI_SUCCESS;
}

extern "C" int
PMI2_KVS_Fence(void)
{
  return PMI_SUCCESS;
}

extern "C" int
PMI2_Abort(void)
{
  SST::Hg::abort("unimplemented: PMI2_Abort");
  return PMI_SUCCESS;
}

static bool pmi_initialized = false;

extern "C" int
PMI2_Job_GetId(char jobid[], int jobid_size)
{
  auto thr = SST::Hg::OperatingSystem::currentThread();
  ::sprintf(jobid, "%d", thr->aid());
  return PMI_SUCCESS;
}

extern "C" int
PMI2_Init(int *spawned, int *size, int *rank, int *appnum)
{
  auto api = sstmac_pmi();
  api->init();
  pmi_initialized = true;
  *size = api->nproc();
  *rank = api->rank();
  *appnum = 0;
  *spawned = 0;
  return PMI_SUCCESS;
}

extern "C" int PMI_Abort(int rc, const char error_msg[])
{
  SST::Hg::abort(error_msg);
  return PMI_SUCCESS;
}

bool pmi_finalized = false;

extern "C" int PMI_Initialized( PMI_BOOL *initialized )
{
  *initialized = pmi_initialized ? 1 : 0;
  return PMI_SUCCESS;
}

extern "C" int
PMI2_Finalize()
{
  pmi_finalized = true;
  pmi_initialized = false;
  auto api = sstmac_pmi();
  api->finish();
  return PMI_SUCCESS;
}

extern "C" int
PMI_Get_nidlist_ptr(void** nidlist)
{
  *nidlist = sstmac_pmi()->nidlist();
  return PMI_SUCCESS;
}

extern "C" int PMI_Init(int* spawned)
{
  auto* tport = sstmac_pmi();
  tport->init();
  pmi_initialized = true;
  *spawned = 0;

  int nproc = tport->nproc();
  char mapping[64];
  snprintf(mapping, sizeof(mapping), "(vector,(%d,%d,%d))", 0, nproc, 1);
  char kvsname[256];
  snprintf(kvsname, sizeof(kvsname), "app%d", tport->sid().app_);
  kvs_lock.lock();
  kvs[kvsname]["PMI_process_mapping"] = mapping;
  kvs_lock.unlock();

  return PMI_SUCCESS;
}

extern "C" int PMI_Finalize()
{
  sstmac_pmi()->finish();
  pmi_initialized = false;
  return PMI_SUCCESS;
}


extern "C" int PMI_Allgather(void *in, void *out, int len)
{
  auto tport = sstmac_pmi();
  int init_tag = tport->engine()->allocateGlobalCollectiveTag();
  auto* msg = tport->engine()->allgather(out, in, len, 1, init_tag, SST::Iris::sumi::Message::default_cq, nullptr);
  if (msg) delete msg;
  return PMI_SUCCESS;
}

extern "C" int PMI_Barrier()
{
  auto api = sstmac_pmi();
  int init_tag = api->engine()->allocateGlobalCollectiveTag();
  api->engine()->barrier(init_tag, SST::Iris::sumi::Message::default_cq, nullptr);
  auto* msg = api->engine()->blockUntilNext(SST::Iris::sumi::Message::default_cq);
  if (msg) delete msg;
  return PMI_SUCCESS;
}

static bool ibarrier_pending = false;

extern "C" int PMI_Ibarrier()
{
  auto api = sstmac_pmi();
  int init_tag = api->engine()->allocateGlobalCollectiveTag();
  api->engine()->barrier(init_tag, SST::Iris::sumi::Message::default_cq, nullptr);
  ibarrier_pending = true;
  return PMI_SUCCESS;
}

extern "C" int PMI_Wait()
{
  if (!ibarrier_pending)
    return PMI_SUCCESS;
  auto api = sstmac_pmi();
  auto* msg = api->engine()->blockUntilNext(SST::Iris::sumi::Message::default_cq);
  if (msg) delete msg;
  ibarrier_pending = false;
  return PMI_SUCCESS;
}

extern "C" int
PMI_Get_numpes_on_smp(int* num)
{
  *num = 1;
  return PMI_SUCCESS;
}

extern "C" int
PMI_Publish_name( const char service_name[], const char port[] )
{
  SST::Hg::abort("unimplemented error: PMI_Publish_name");
  return PMI_SUCCESS;
}

extern "C" int
PMI_Unpublish_name( const char service_name[] )
{
  SST::Hg::abort("unimplemented error: PMI_Unpublish_name");
  return PMI_SUCCESS;
}

extern "C" int
PMI_KVS_Get_name_length_max( int *length )
{
  *length = 256;
  return PMI_SUCCESS;
}

extern "C" int
PMI_KVS_Get_key_length_max( int *length )
{
  *length = 256;
  return PMI_SUCCESS;
}

extern "C" int
PMI_KVS_Get_value_length_max( int *length )
{
  *length = 256;
  return PMI_SUCCESS;
}

extern "C" int
PMI_KVS_Get_my_name( char kvsname[], int length )
{
  auto* api = sstmac_pmi();
  int actual_length = snprintf(kvsname, length, "app%d", api->sid().app_);
  if (actual_length >= length){
    return PMI_ERR_INVALID_LENGTH;
  } else {
    return PMI_SUCCESS;
  }
}

extern "C" int
PMI_Spawn_multiple(int count,
                       const char * cmds[],
                       const char ** argvs[],
                       const int maxprocs[],
                       const int info_keyval_sizesp[],
                       const PMI_keyval_t * info_keyval_vectors[],
                       int preput_keyval_size,
                       const PMI_keyval_t preput_keyval_vector[],
                       int errors[])
{
  SST::Hg::abort("unimplemented error: PMI_Spawn_multiple");
  return PMI_SUCCESS;
}

extern "C" int
PMI_Lookup_name( const char service_name[], char port[] )
{
  SST::Hg::abort("unimplemented error: PMI_Lookup_name");
  return PMI_SUCCESS;
}

extern "C" int
PMI_KVS_Commit( const char kvsname[] )
{
  return PMI_SUCCESS;
}

extern "C" int
PMI_Get_universe_size( int *size )
{
  auto* api = sstmac_pmi();
  *size = api->nproc();
  return PMI_SUCCESS;
}

extern "C" int
PMI_Get_appnum( int *appnum )
{
  auto* api = sstmac_pmi();
  *appnum = api->sid().app_;
  return PMI_SUCCESS;
}
