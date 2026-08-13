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

/*
 * FI_COLLECTIVE bridge for the sumi provider.
 *
 * This wires libfabric's fi_ops_collective (fi_allreduce / fi_barrier) to the
 * sumi CollectiveEngine, which routes the request through the sumi
 * CollectiveRegistry -- the SAME registry consulted by the mask-mpi and (in
 * principle) MVAPICH2 engine paths. So the algorithm actually simulated is
 * selected by the ordinary knobs:
 *
 *     param  app1.collective.allreduce=<name> (e.g. "ring", "recdouble")
 *     env    SUMI_ALLREDUCE_ALG=<name>
 *     else   the engine's built-in default
 *
 * An OFI client (e.g. MVAPICH2 ch4:ofi built to offload collectives to the
 * provider) transparently gets the registry-selected native algorithm, modeled
 * at packet level, instead of libfabric's generic point-to-point util_coll.
 *
 * Current limitations:
 *   - Operations are cooperatively blocking inside the provider, then post one
 *     completion to the endpoint's send CQ.
 *   - Reduce-scatter and all-to-all are reported as unsupported.
 *   - Only FI_SUM/FI_PROD/FI_MIN/FI_MAX over the common fixed-width datatypes
 *     are mapped; anything else returns -FI_EOPNOTSUPP.
 */

#include "sumi_prov.h"
#include "sumi_ep.h"

#include <sumi_fabric.hpp>
#include <sumi/transport.h>
#include <sumi/comm_functions.h>
#include <sumi/communicator.h>
#include <mercury/components/operating_system.h>

#include <climits>
#include <new>
#include <vector>

using SST::Iris::sumi::CollectiveEngine;
using SST::Iris::sumi::CollectiveDoneMessage;
using SST::Iris::sumi::Collective;
using SST::Iris::sumi::Communicator;
using SST::Iris::sumi::MapCommunicator;
using SST::Iris::sumi::Message;
using SST::Iris::sumi::reduce_fxn;
using SST::Iris::sumi::ReduceOp;
using SST::Iris::sumi::Add;
using SST::Iris::sumi::Prod;
using SST::Iris::sumi::Min;
using SST::Iris::sumi::Max;

namespace {

uint64_t membershipKey(const sumi_fid_av_set* set)
{
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < set->count; ++i) {
    uint64_t addr = set->addrs[i];
    for (unsigned shift = 0; shift < 64; shift += 8) {
      hash ^= (addr >> shift) & 0xff;
      hash *= 1099511628211ULL;
    }
  }
  return hash;
}

int sumi_mc_close(fid_t fid)
{
  auto* mc = reinterpret_cast<sumi_fid_mc*>(fid);
  delete static_cast<Communicator*>(mc->communicator);
  free(mc->members);
  free(mc);
  return FI_SUCCESS;
}

struct fi_ops sumi_mc_ops = {
  .size = sizeof(struct fi_ops),
  .close = sumi_mc_close,
  .bind = fi_no_bind,
  .control = fi_no_control,
  .ops_open = fi_no_ops_open,
};

int datatypeSize(enum fi_datatype dt)
{
  switch (dt) {
    case FI_INT32:
    case FI_UINT32:
    case FI_FLOAT:  return 4;
    case FI_INT64:
    case FI_UINT64:
    case FI_DOUBLE: return 8;
    default:        return 0;
  }
}

size_t maxEngineCount(size_t type_size, size_t members = 1)
{
  if (!type_size || !members) return 0;
  return static_cast<size_t>(INT_MAX) / type_size / members;
}

int engineCount(size_t count, size_t type_size, size_t members, int* result)
{
  if (!result || count > maxEngineCount(type_size, members))
    return -FI_EMSGSIZE;
  *result = static_cast<int>(count);
  return FI_SUCCESS;
}

template <template <typename> class Op>
reduce_fxn fxnForType(enum fi_datatype dt)
{
  switch (dt) {
    case FI_INT32:  return &ReduceOp<Op, int32_t>::op;
    case FI_UINT32: return &ReduceOp<Op, uint32_t>::op;
    case FI_INT64:  return &ReduceOp<Op, int64_t>::op;
    case FI_UINT64: return &ReduceOp<Op, uint64_t>::op;
    case FI_FLOAT:  return &ReduceOp<Op, float>::op;
    case FI_DOUBLE: return &ReduceOp<Op, double>::op;
    default:        return nullptr;
  }
}

// Map (fi_op, fi_datatype) to the engine's reduce function. Only the ops that
// are meaningful for every mapped datatype are handled, so no bitwise op is
// instantiated for float/double.
reduce_fxn selectFxn(enum fi_op op, enum fi_datatype dt)
{
  switch (op) {
    case FI_SUM:  return fxnForType<Add>(dt);
    case FI_PROD: return fxnForType<Prod>(dt);
    case FI_MIN:  return fxnForType<Min>(dt);
    case FI_MAX:  return fxnForType<Max>(dt);
    default:      return nullptr;
  }
}

int finishCollective(CollectiveEngine* engine, CollectiveDoneMessage* done,
                     int cq, int tag,
                     Collective::type_t type, Communicator* comm)
{
  if (!done) done = engine->blockUntilNext(cq);
  if (!done) return -FI_EIO;
  const bool matches = done->tag() == tag && done->type() == type &&
                       done->dom() == comm;
  delete done;
  return matches ? FI_SUCCESS : -FI_EIO;
}

struct CollectiveCall {
  sumi_fid_ep* ep;
  FabricTransport* tport;
  CollectiveEngine* engine;
  Communicator* comm;
  sumi_fid_mc* mc;
};

int getCollective(struct fid_ep* ep_, fi_addr_t coll_addr, CollectiveCall* call)
{
  if (!ep_ || !call || coll_addr == FI_ADDR_UNSPEC) return -FI_EINVAL;
  auto* ep = reinterpret_cast<sumi_fid_ep*>(ep_);
  if (!ep->send_cq) return -FI_ENOCQ;
  auto* mc = reinterpret_cast<sumi_fid_mc*>(static_cast<uintptr_t>(coll_addr));
  if (!mc || mc->mc_fid.fid.fclass != FI_CLASS_MC ||
      mc->mc_fid.fi_addr != coll_addr || mc->ep != ep || !mc->communicator)
    return -FI_EINVAL;
  auto* tport = reinterpret_cast<FabricTransport*>(ep->domain->fabric->tport);
  auto* engine = tport->engine();
  if (!engine) return -FI_EOPNOTSUPP;
  call->ep = ep;
  call->tport = tport;
  call->engine = engine;
  call->comm = static_cast<Communicator*>(mc->communicator);
  call->mc = mc;
  return FI_SUCCESS;
}

int rootRank(const CollectiveCall& call, fi_addr_t root_addr)
{
  for (size_t i = 0; i < call.mc->member_count; ++i) {
    if (call.mc->members[i] == root_addr) return static_cast<int>(i);
  }
  return -1;
}

int requireAccess(const CollectiveCall& call, uint64_t required)
{
  return (call.mc->access & required) == required ? FI_SUCCESS : -FI_EACCES;
}

int nextTag(const CollectiveCall& call, int* tag)
{
  return call.tport->nextCollectiveTag(call.mc->group_key, tag)
      ? FI_SUCCESS : -FI_EOVERFLOW;
}

// Deliver a single OFI CQ completion for a (blocking) collective by looping a
// zero-byte short message back to ourselves, delivered to the endpoint's send
// CQ (remote_cq = send_cq->id). We deliberately do NOT request a local ack
// (local_cq = no_ack): Message::cloneInjectionAck() slices a FabricMessage down
// to its base, so an ack could not carry the FabricMessage fields
// (context/flags) the CQ reader expects. An smsg is not a posted_send, so
// RecvQueue::incoming routes it straight to the CQ progress queue -- intact
// derived type -- and the client's next fi_cq_read observes it. The completion
// carries the user context; its length is reported as zero.
void deliverCompletion(sumi_fid_ep* ep, void* context, uint64_t role_flags)
{
  if (!ep->send_cq) return;
  FabricTransport* tport = (FabricTransport*) ep->domain->fabric->tport;
  tport->smsgSend<FabricMessage>(
      /*remote_proc*/ tport->rank(), /*byte_length*/ 0, /*buffer*/ nullptr,
      /*local_cq*/ Message::no_ack, /*remote_cq*/ ep->send_cq->id,
      Message::pt2pt, /*qos*/ ep->qos,
      FabricMessage::no_tag, FI_COLLECTIVE | role_flags,
      FabricMessage::no_imm_data,
      context);
}

} // namespace

extern "C" int sumi_query_collective(struct fid_domain* domain,
                                      enum fi_collective_op coll,
                                      struct fi_collective_attr* attr,
                                      uint64_t flags)
{
  if (!domain || !attr || flags || attr->mode) return -FI_EINVAL;

  auto* dom = reinterpret_cast<sumi_fid_domain*>(domain);
  auto* tport = reinterpret_cast<FabricTransport*>(dom->fabric->tport);
  int type_size = 0;
  size_t member_factor = 1;
  switch (coll) {
    case FI_BARRIER:
      break;
    case FI_ALLREDUCE:
    case FI_REDUCE:
      type_size = datatypeSize(attr->datatype);
      if (!type_size || !selectFxn(attr->op, attr->datatype))
        return -FI_EOPNOTSUPP;
      break;
    case FI_BROADCAST:
      type_size = datatypeSize(attr->datatype);
      if (!type_size) return -FI_EOPNOTSUPP;
      break;
    case FI_ALLGATHER:
    case FI_SCATTER:
    case FI_GATHER:
      type_size = datatypeSize(attr->datatype);
      if (!type_size) return -FI_EOPNOTSUPP;
      member_factor = static_cast<size_t>(tport->nproc());
      break;
    case FI_ALLTOALL:
    case FI_REDUCE_SCATTER:
    default:
      return -FI_EOPNOTSUPP;
  }

  attr->max_members = static_cast<size_t>(tport->nproc());
  attr->datatype_attr.count = type_size
      ? maxEngineCount(static_cast<size_t>(type_size), member_factor)
      : 0;
  attr->datatype_attr.size = static_cast<size_t>(type_size);
  return FI_SUCCESS;
}

extern "C" int sumi_join_collective(struct fid_ep* ep_, const void* addr_,
                                     uint64_t flags, struct fid_mc** mc_out,
                                     void* context)
{
  constexpr uint64_t allowed_flags = FI_COLLECTIVE | FI_SEND | FI_RECV;
  if (!ep_ || !addr_ || !mc_out || !(flags & FI_COLLECTIVE) ||
      (flags & ~allowed_flags))
    return -FI_EINVAL;

  auto* ep = reinterpret_cast<sumi_fid_ep*>(ep_);
  if (!ep->eq) return -FI_ENOEQ;
  const auto* addr = static_cast<const fi_collective_addr*>(addr_);
  if (!addr->set || addr->set->fid.fclass != FI_CLASS_AV_SET)
    return -FI_EINVAL;
  if (addr->coll_addr != FI_ADDR_NOTAVAIL) return -FI_EOPNOTSUPP;
  auto* set = reinterpret_cast<sumi_fid_av_set*>(
      const_cast<fid_av_set*>(addr->set));
  if (!set->count || !ep->av || set->av != ep->av) return -FI_EINVAL;

  auto* tport = reinterpret_cast<FabricTransport*>(ep->domain->fabric->tport);
  const int my_global_rank = tport->rank();
  int my_comm_rank = -1;
  std::vector<int> ranks;
  std::vector<bool> seen_ranks(static_cast<size_t>(tport->nproc()), false);
  ranks.reserve(set->count);
  for (size_t i = 0; i < set->count; ++i) {
    int rank = static_cast<int>(ADDR_RANK(set->addrs[i]));
    if (rank < 0 || rank >= tport->nproc()) return -FI_EINVAL;
    if (seen_ranks[rank]) return -FI_EINVAL;
    seen_ranks[rank] = true;
    if (rank == my_global_rank) my_comm_rank = static_cast<int>(i);
    ranks.push_back(rank);
  }
  if (my_comm_rank < 0) return -FI_EINVAL;

  auto* mc = static_cast<sumi_fid_mc*>(calloc(1, sizeof(sumi_fid_mc)));
  if (!mc) return -FI_ENOMEM;
  if (set->count > SIZE_MAX / sizeof(fi_addr_t)) {
    free(mc);
    return -FI_EOVERFLOW;
  }
  mc->members = static_cast<fi_addr_t*>(
      malloc(set->count * sizeof(fi_addr_t)));
  if (!mc->members) {
    free(mc);
    return -FI_ENOMEM;
  }
  memcpy(mc->members, set->addrs, set->count * sizeof(fi_addr_t));
  mc->member_count = set->count;
  mc->group_key = tport->nextCollectiveGroupKey(membershipKey(set));
  mc->communicator = new (std::nothrow)
      MapCommunicator(my_comm_rank, std::move(ranks), mc->group_key);
  if (!mc->communicator) {
    free(mc->members);
    free(mc);
    return -FI_ENOMEM;
  }
  mc->mc_fid.fid.fclass = FI_CLASS_MC;
  mc->mc_fid.fid.context = context;
  mc->mc_fid.fid.ops = &sumi_mc_ops;
  mc->mc_fid.fi_addr = reinterpret_cast<fi_addr_t>(mc);
  mc->ep = ep;
  mc->access = flags & (FI_SEND | FI_RECV);
  if (!mc->access) mc->access = FI_SEND | FI_RECV;

  fi_eq_entry entry = {};
  entry.fid = &mc->mc_fid.fid;
  entry.context = context;
  ssize_t written = fi_eq_write(&ep->eq->eq_fid, FI_JOIN_COMPLETE, &entry,
                                sizeof(entry), FI_COLLECTIVE);
  if (written != static_cast<ssize_t>(sizeof(entry))) {
    sumi_mc_close(&mc->mc_fid.fid);
    return written < 0 ? static_cast<int>(written) : -FI_EIO;
  }
  *mc_out = &mc->mc_fid;
  return FI_SUCCESS;
}

static ssize_t sumi_ep_allreduce(struct fid_ep *ep_, const void *buf, size_t count,
                                 void * /*desc*/, void *result, void * /*result_desc*/,
                                 fi_addr_t coll_addr, enum fi_datatype datatype,
                                 enum fi_op op, uint64_t flags, void *context)
{
  CollectiveCall call;
  int ret = getCollective(ep_, coll_addr, &call);
  if (ret) return ret;
  if (flags) return -FI_EINVAL;
  ret = requireAccess(call, FI_SEND | FI_RECV);
  if (ret) return ret;

  int type_size = datatypeSize(datatype);
  reduce_fxn fxn = selectFxn(op, datatype);
  if (!type_size || !fxn) return -FI_EOPNOTSUPP;
  int nelems;
  ret = engineCount(count, static_cast<size_t>(type_size), 1, &nelems);
  if (ret) return ret;

  // Tags are shared across endpoints and scoped by joined group membership.
  int tag;
  ret = nextTag(call, &tag);
  if (ret) return ret;
  int cq = Message::collective_cq;

  // The registry override inside CollectiveEngine::allreduce picks the actual
  // algorithm from collective.allreduce / SUMI_ALLREDUCE_ALG.
  auto* done = call.engine->allreduce(result, const_cast<void*>(buf), nelems,
                                      type_size, tag, fxn, cq,
                                      call.comm);
  ret = finishCollective(call.engine, done, cq, tag,
                         Collective::allreduce, call.comm);
  if (ret) return ret;

  deliverCompletion(call.ep, context, FI_SEND | FI_RECV);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_barrier(struct fid_ep *ep_, fi_addr_t coll_addr, void *context)
{
  CollectiveCall call;
  int ret = getCollective(ep_, coll_addr, &call);
  if (ret) return ret;

  int tag;
  ret = nextTag(call, &tag);
  if (ret) return ret;
  int cq = Message::collective_cq;
  auto* done = call.engine->barrier(tag, cq, call.comm);
  ret = finishCollective(call.engine, done, cq, tag,
                         Collective::barrier, call.comm);
  if (ret) return ret;

  deliverCompletion(call.ep, context, FI_SEND);
  return FI_SUCCESS;
}

// Resolve the joined group and engine consistently for each operation.
#define COLL_PROLOGUE(EPV, ADDR)                 \
  CollectiveCall call;                           \
  int call_ret = getCollective((EPV), (ADDR), &call); \
  if (call_ret) return call_ret

static ssize_t sumi_ep_broadcast(struct fid_ep *ep_, void *buf, size_t count,
                                 void * /*desc*/, fi_addr_t coll_addr,
                                 fi_addr_t root_addr, enum fi_datatype datatype,
                                 uint64_t flags, void *context)
{
  COLL_PROLOGUE(ep_, coll_addr);
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int nelems;
  call_ret = engineCount(count, static_cast<size_t>(type_size), 1, &nelems);
  if (call_ret) return call_ret;
  int root = rootRank(call, root_addr);
  if (root < 0) return -FI_EINVAL;
  const uint64_t expected = call.comm->myCommRank() == root ? FI_SEND : FI_RECV;
  if (flags != expected) return -FI_EINVAL;
  call_ret = requireAccess(call, expected);
  if (call_ret) return call_ret;
  int tag;
  call_ret = nextTag(call, &tag);
  if (call_ret) return call_ret;
  int cq = Message::collective_cq;
  auto* done = call.engine->bcast(root, buf, nelems, type_size, tag,
                                  cq, call.comm);
  call_ret = finishCollective(call.engine, done, cq, tag,
                              Collective::bcast, call.comm);
  if (call_ret) return call_ret;
  deliverCompletion(call.ep, context, expected);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_reduce(struct fid_ep *ep_, const void *buf, size_t count,
                              void * /*desc*/, void *result, void * /*result_desc*/,
                              fi_addr_t coll_addr, fi_addr_t root_addr,
                              enum fi_datatype datatype, enum fi_op op,
                              uint64_t flags, void *context)
{
  COLL_PROLOGUE(ep_, coll_addr);
  if (flags) return -FI_EINVAL;
  int type_size = datatypeSize(datatype);
  reduce_fxn fxn = selectFxn(op, datatype);
  if (!type_size || !fxn) return -FI_EOPNOTSUPP;
  int nelems;
  call_ret = engineCount(count, static_cast<size_t>(type_size), 1, &nelems);
  if (call_ret) return call_ret;
  int root = rootRank(call, root_addr);
  if (root < 0) return -FI_EINVAL;
  const uint64_t required = call.comm->myCommRank() == root
      ? FI_SEND | FI_RECV : FI_SEND;
  call_ret = requireAccess(call, required);
  if (call_ret) return call_ret;
  int tag;
  call_ret = nextTag(call, &tag);
  if (call_ret) return call_ret;
  int cq = Message::collective_cq;
  auto* done = call.engine->reduce(root, result, const_cast<void*>(buf),
                                   nelems, type_size, tag, fxn,
                                   cq, call.comm);
  call_ret = finishCollective(call.engine, done, cq, tag,
                              Collective::reduce, call.comm);
  if (call_ret) return call_ret;
  deliverCompletion(call.ep, context, required);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_allgather(struct fid_ep *ep_, const void *buf, size_t count,
                                 void * /*desc*/, void *result, void * /*result_desc*/,
                                 fi_addr_t coll_addr, enum fi_datatype datatype,
                                 uint64_t flags, void *context)
{
  COLL_PROLOGUE(ep_, coll_addr);
  if (flags) return -FI_EINVAL;
  call_ret = requireAccess(call, FI_SEND | FI_RECV);
  if (call_ret) return call_ret;
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int nelems;
  call_ret = engineCount(count, static_cast<size_t>(type_size),
                         static_cast<size_t>(call.comm->nproc()), &nelems);
  if (call_ret) return call_ret;
  int tag;
  call_ret = nextTag(call, &tag);
  if (call_ret) return call_ret;
  int cq = Message::collective_cq;
  auto* done = call.engine->allgather(result, const_cast<void*>(buf), nelems,
                                      type_size, tag, cq,
                                      call.comm);
  call_ret = finishCollective(call.engine, done, cq, tag,
                              Collective::allgather, call.comm);
  if (call_ret) return call_ret;
  deliverCompletion(call.ep, context, FI_SEND | FI_RECV);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_gather(struct fid_ep *ep_, const void *buf, size_t count,
                              void * /*desc*/, void *result, void * /*result_desc*/,
                              fi_addr_t coll_addr, fi_addr_t root_addr,
                              enum fi_datatype datatype, uint64_t flags,
                              void *context)
{
  COLL_PROLOGUE(ep_, coll_addr);
  if (flags) return -FI_EINVAL;
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int nelems;
  call_ret = engineCount(count, static_cast<size_t>(type_size),
                         static_cast<size_t>(call.comm->nproc()), &nelems);
  if (call_ret) return call_ret;
  int root = rootRank(call, root_addr);
  if (root < 0) return -FI_EINVAL;
  const uint64_t required = call.comm->myCommRank() == root
      ? FI_SEND | FI_RECV : FI_SEND;
  call_ret = requireAccess(call, required);
  if (call_ret) return call_ret;
  int tag;
  call_ret = nextTag(call, &tag);
  if (call_ret) return call_ret;
  int cq = Message::collective_cq;
  auto* done = call.engine->gather(root, result, const_cast<void*>(buf),
                                   nelems, type_size, tag,
                                   cq, call.comm);
  call_ret = finishCollective(call.engine, done, cq, tag,
                              Collective::gather, call.comm);
  if (call_ret) return call_ret;
  deliverCompletion(call.ep, context, required);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_scatter(struct fid_ep *ep_, const void *buf, size_t count,
                               void * /*desc*/, void *result, void * /*result_desc*/,
                               fi_addr_t coll_addr, fi_addr_t root_addr,
                               enum fi_datatype datatype, uint64_t flags,
                               void *context)
{
  COLL_PROLOGUE(ep_, coll_addr);
  if (flags) return -FI_EINVAL;
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int nelems;
  call_ret = engineCount(count, static_cast<size_t>(type_size),
                         static_cast<size_t>(call.comm->nproc()), &nelems);
  if (call_ret) return call_ret;
  int root = rootRank(call, root_addr);
  if (root < 0) return -FI_EINVAL;
  const uint64_t required = call.comm->myCommRank() == root
      ? FI_SEND | FI_RECV : FI_RECV;
  call_ret = requireAccess(call, required);
  if (call_ret) return call_ret;
  int tag;
  call_ret = nextTag(call, &tag);
  if (call_ret) return call_ret;
  int cq = Message::collective_cq;
  auto* done = call.engine->scatter(root, result, const_cast<void*>(buf),
                                    nelems, type_size, tag,
                                    cq, call.comm);
  call_ret = finishCollective(call.engine, done, cq, tag,
                              Collective::scatter, call.comm);
  if (call_ret) return call_ret;
  deliverCompletion(call.ep, context, required);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_reduce_scatter(struct fid_ep* /*ep*/, const void* /*buf*/,
                                      size_t /*count*/, void* /*desc*/,
                                      void* /*result*/, void* /*result_desc*/,
                                      fi_addr_t /*coll_addr*/,
                                      enum fi_datatype /*datatype*/,
                                      enum fi_op /*op*/, uint64_t /*flags*/,
                                      void* /*context*/)
{
  return -FI_EOPNOTSUPP;
}

struct fi_ops_collective sumi_ep_collective_ops = {
  .size = sizeof(struct fi_ops_collective),
  .barrier = sumi_ep_barrier,
  .broadcast = sumi_ep_broadcast,
  .alltoall = fi_coll_no_alltoall,
  .allreduce = sumi_ep_allreduce,
  .allgather = sumi_ep_allgather,
  .reduce_scatter = sumi_ep_reduce_scatter,
  .reduce = sumi_ep_reduce,
  .scatter = sumi_ep_scatter,
  .gather = sumi_ep_gather,
  .msg = fi_coll_no_msg,
};
