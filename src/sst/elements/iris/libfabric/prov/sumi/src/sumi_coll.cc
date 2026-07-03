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
 * FI_COLLECTIVE bridge for the sumi provider (spike).
 *
 * This wires libfabric's fi_ops_collective (fi_allreduce / fi_barrier) to the
 * sumi CollectiveEngine, which routes the request through the sumi
 * CollectiveRegistry -- the SAME registry consulted by the mask-mpi and (in
 * principle) MVAPICH2 engine paths. So the algorithm actually simulated is
 * selected by the ordinary knobs:
 *
 *     param  app1.allreduce_alg=<name>     (e.g. "ring", "recdouble")
 *     env    SUMI_ALLREDUCE_ALG=<name>
 *     else   the engine's built-in default
 *
 * That is the whole point of the spike: prove OFI -> engine -> registry. An
 * OFI client (e.g. MVAPICH2 ch4:ofi built to offload collectives to the
 * provider) transparently gets the registry-selected native algorithm, modeled
 * at packet level, instead of libfabric's generic point-to-point util_coll.
 *
 * SPIKE SIMPLIFICATIONS (call out honestly; see sumi-fi-collective.md):
 *   - coll_addr selects the group: FI_ADDR_UNSPEC = the world communicator
 *     (engine global domain); otherwise it is a sub-communicator built by
 *     fi_join_collective (see sumi_cm_join / sumi_av_set below).
 *   - The op is cooperatively blocking: it issues on the engine's collective
 *     cq and blockUntilNext()-es to completion before returning, exactly like
 *     the transport's own bootstrap barrier (SimTransport::init). It then posts
 *     a single completion to the endpoint's send CQ so a polling client sees
 *     it. A fully non-blocking implementation would instead route the engine's
 *     CollectiveDoneMessage cq onto the OFI CQ.
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

#include <algorithm>
#include <vector>

using SST::Iris::sumi::CollectiveEngine;
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

// coll_addr passed to the collective ops is either FI_ADDR_UNSPEC (the world
// communicator = engine global domain, encoded as nullptr) or the pointer to a
// sumi Communicator produced by fi_join_collective (see sumi_cm_join). Encoding
// the Communicator* directly in the multicast fi_addr avoids any per-rank
// registry / global state.
Communicator* collComm(fi_addr_t coll_addr)
{
  if (coll_addr == FI_ADDR_UNSPEC) return nullptr;
  return reinterpret_cast<Communicator*>((uintptr_t) coll_addr);
}

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

// Deliver a single OFI CQ completion for a (blocking) collective by looping a
// zero-byte short message back to ourselves, delivered to the endpoint's send
// CQ (remote_cq = send_cq->id). We deliberately do NOT request a local ack
// (local_cq = no_ack): Message::cloneInjectionAck() slices a FabricMessage down
// to its base, so an ack could not carry the FabricMessage fields
// (context/flags) the CQ reader expects. An smsg is not a posted_send, so
// RecvQueue::incoming routes it straight to the CQ progress queue -- intact
// derived type -- and the client's next fi_cq_read observes it. The completion
// carries the user context; its length is reported as zero (spike).
void deliverCompletion(sumi_fid_ep* ep, void* context)
{
  if (!ep->send_cq) return;
  FabricTransport* tport = (FabricTransport*) ep->domain->fabric->tport;
  tport->smsgSend<FabricMessage>(
      /*remote_proc*/ tport->rank(), /*byte_length*/ 0, /*buffer*/ nullptr,
      /*local_cq*/ Message::no_ack, /*remote_cq*/ ep->send_cq->id,
      Message::pt2pt, /*qos*/ ep->qos,
      FabricMessage::no_tag, FI_COLLECTIVE | FI_SEND, FabricMessage::no_imm_data,
      context);
}

} // namespace

static ssize_t sumi_ep_allreduce(struct fid_ep *ep_, const void *buf, size_t count,
                                 void * /*desc*/, void *result, void * /*result_desc*/,
                                 fi_addr_t coll_addr, enum fi_datatype datatype,
                                 enum fi_op op, uint64_t /*flags*/, void *context)
{
  sumi_fid_ep* ep = (sumi_fid_ep*) ep_;
  FabricTransport* tport = (FabricTransport*) ep->domain->fabric->tport;
  CollectiveEngine* engine = tport->engine();
  if (!engine) return -FI_EOPNOTSUPP;

  int type_size = datatypeSize(datatype);
  reduce_fxn fxn = selectFxn(op, datatype);
  if (!type_size || !fxn) return -FI_EOPNOTSUPP;

  // All ranks issue collectives in the same order (SPMD), so a per-endpoint
  // monotonic tag agrees across ranks for a given collective instance.
  int tag = ep->coll_tag++;

  // comm == nullptr -> engine global domain (the world communicator).
  // The registry override inside CollectiveEngine::allreduce picks the actual
  // algorithm from allreduce_alg / SUMI_ALLREDUCE_ALG.
  engine->allreduce(result, const_cast<void*>(buf), (int) count, type_size, tag, fxn,
                    Message::default_cq, /*comm=*/collComm(coll_addr));
  engine->blockUntilNext(Message::default_cq);

  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_barrier(struct fid_ep *ep_, fi_addr_t coll_addr, void *context)
{
  sumi_fid_ep* ep = (sumi_fid_ep*) ep_;
  FabricTransport* tport = (FabricTransport*) ep->domain->fabric->tport;
  CollectiveEngine* engine = tport->engine();
  if (!engine) return -FI_EOPNOTSUPP;

  int tag = ep->coll_tag++;
  engine->barrier(tag, Message::default_cq, /*comm=*/collComm(coll_addr));
  engine->blockUntilNext(Message::default_cq);

  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

// The engine + registry live behind ep->domain->fabric->tport; every op grabs
// them the same way. `root_addr` is interpreted as a raw rank index: without
// fi_join_collective there is no AV mapping, so the client passes the root's
// rank directly (spike).
#define COLL_PROLOGUE(EPV)                                          \
  sumi_fid_ep* ep = (sumi_fid_ep*) (EPV);                           \
  FabricTransport* tport = (FabricTransport*) ep->domain->fabric->tport; \
  CollectiveEngine* engine = tport->engine();                      \
  if (!engine) return -FI_EOPNOTSUPP;

static ssize_t sumi_ep_broadcast(struct fid_ep *ep_, void *buf, size_t count,
                                 void * /*desc*/, fi_addr_t coll_addr,
                                 fi_addr_t root_addr, enum fi_datatype datatype,
                                 uint64_t /*flags*/, void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->bcast((int) root_addr, buf, (int) count, type_size, tag,
                Message::default_cq, /*comm=*/collComm(coll_addr));
  engine->blockUntilNext(Message::default_cq);
  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_reduce(struct fid_ep *ep_, const void *buf, size_t count,
                              void * /*desc*/, void *result, void * /*result_desc*/,
                              fi_addr_t coll_addr, fi_addr_t root_addr,
                              enum fi_datatype datatype, enum fi_op op,
                              uint64_t /*flags*/, void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  reduce_fxn fxn = selectFxn(op, datatype);
  if (!type_size || !fxn) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->reduce((int) root_addr, result, const_cast<void*>(buf), (int) count,
                 type_size, tag, fxn, Message::default_cq, /*comm=*/collComm(coll_addr));
  engine->blockUntilNext(Message::default_cq);
  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_allgather(struct fid_ep *ep_, const void *buf, size_t count,
                                 void * /*desc*/, void *result, void * /*result_desc*/,
                                 fi_addr_t coll_addr, enum fi_datatype datatype,
                                 uint64_t /*flags*/, void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->allgather(result, const_cast<void*>(buf), (int) count, type_size, tag,
                    Message::default_cq, /*comm=*/collComm(coll_addr));
  engine->blockUntilNext(Message::default_cq);
  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_gather(struct fid_ep *ep_, const void *buf, size_t count,
                              void * /*desc*/, void *result, void * /*result_desc*/,
                              fi_addr_t coll_addr, fi_addr_t root_addr,
                              enum fi_datatype datatype, uint64_t /*flags*/,
                              void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->gather((int) root_addr, result, const_cast<void*>(buf), (int) count,
                 type_size, tag, Message::default_cq, /*comm=*/collComm(coll_addr));
  engine->blockUntilNext(Message::default_cq);
  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_scatter(struct fid_ep *ep_, const void *buf, size_t count,
                               void * /*desc*/, void *result, void * /*result_desc*/,
                               fi_addr_t coll_addr, fi_addr_t root_addr,
                               enum fi_datatype datatype, uint64_t /*flags*/,
                               void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->scatter((int) root_addr, result, const_cast<void*>(buf), (int) count,
                  type_size, tag, Message::default_cq, /*comm=*/collComm(coll_addr));
  engine->blockUntilNext(Message::default_cq);
  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_reduce_scatter(struct fid_ep *ep_, const void *buf, size_t count,
                                      void * /*desc*/, void *result, void * /*result_desc*/,
                                      fi_addr_t coll_addr, enum fi_datatype datatype,
                                      enum fi_op op, uint64_t /*flags*/, void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  reduce_fxn fxn = selectFxn(op, datatype);
  if (!type_size || !fxn) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->reduceScatter(result, const_cast<void*>(buf), (int) count, type_size,
                        tag, fxn, Message::default_cq, /*comm=*/collComm(coll_addr));
  engine->blockUntilNext(Message::default_cq);
  deliverCompletion(ep, context);
  return FI_SUCCESS;
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

// ---------------------------------------------------------------------------
// FI_COLLECTIVE group construction: fi_av_set + fi_join_collective.
//
// A client builds an address set (fi_av_set_insert of member fi_addrs), then
// fi_join_collective turns it into a sumi MapCommunicator over those global
// ranks. The Communicator pointer is stored as the multicast fi_addr and handed
// back to the collective ops via coll_addr (decoded by collComm above).
// ---------------------------------------------------------------------------

namespace {

// The fid_av_set must be the first member so a fid_av_set* casts to SumiAvSet*.
struct SumiAvSet {
  struct fid_av_set av_set_fid;
  std::vector<int> ranks;   // member global ranks (fi_addr -> ADDR_RANK)
};

int avset_insert(struct fid_av_set* s, fi_addr_t addr) {
  ((SumiAvSet*) s)->ranks.push_back((int) ADDR_RANK(addr));
  return FI_SUCCESS;
}

int avset_remove(struct fid_av_set* s, fi_addr_t addr) {
  auto& r = ((SumiAvSet*) s)->ranks;
  int want = (int) ADDR_RANK(addr);
  r.erase(std::remove(r.begin(), r.end(), want), r.end());
  return FI_SUCCESS;
}

int avset_union(struct fid_av_set* d, const struct fid_av_set* s) {
  auto& dst = ((SumiAvSet*) d)->ranks;
  const auto& src = ((const SumiAvSet*) s)->ranks;
  dst.insert(dst.end(), src.begin(), src.end());
  return FI_SUCCESS;
}

int avset_intersect(struct fid_av_set*, const struct fid_av_set*) { return -FI_ENOSYS; }
int avset_diff(struct fid_av_set*, const struct fid_av_set*) { return -FI_ENOSYS; }

int avset_addr(struct fid_av_set*, fi_addr_t* coll_addr) {
  // Membership travels in the set object; join() consumes it directly. Hand
  // back a non-UNSPEC placeholder so a caller passing this as the base
  // coll_addr does not read it as "world".
  if (coll_addr) *coll_addr = 0;
  return FI_SUCCESS;
}

int avset_close(struct fid* fid) { delete (SumiAvSet*) fid; return FI_SUCCESS; }

struct fi_ops_av_set sumi_av_set_ops = {
  .size = sizeof(struct fi_ops_av_set),
  .set_union = avset_union,
  .intersect = avset_intersect,
  .diff = avset_diff,
  .insert = avset_insert,
  .remove = avset_remove,
  .addr = avset_addr,
};

struct fi_ops sumi_av_set_fi_ops = {
  .size = sizeof(struct fi_ops),
  .close = avset_close,
  .bind = fi_no_bind,
  .control = fi_no_control,
  .ops_open = fi_no_ops_open,
};

int mc_close(struct fid* fid) {
  struct fid_mc* m = (struct fid_mc*) fid;
  delete reinterpret_cast<Communicator*>((uintptr_t) m->fi_addr);
  free(m);
  return FI_SUCCESS;
}

struct fi_ops sumi_mc_fi_ops = {
  .size = sizeof(struct fi_ops),
  .close = mc_close,
  .bind = fi_no_bind,
  .control = fi_no_control,
  .ops_open = fi_no_ops_open,
};

} // namespace

int sumi_av_set(struct fid_av* /*av*/, struct fi_av_set_attr* /*attr*/,
                struct fid_av_set** av_set, void* context) {
  // Start empty; the client populates membership via fi_av_set_insert. (The
  // attr start/count/stride range form is not needed for the spike.)
  SumiAvSet* set = new SumiAvSet();
  set->av_set_fid.fid.fclass = FI_CLASS_AV_SET;
  set->av_set_fid.fid.context = context;
  set->av_set_fid.fid.ops = &sumi_av_set_fi_ops;
  set->av_set_fid.ops = &sumi_av_set_ops;
  *av_set = &set->av_set_fid;
  return FI_SUCCESS;
}

int sumi_cm_join(struct fid_ep* ep_, const void* addr, uint64_t /*flags*/,
                 struct fid_mc** mc, void* context) {
  sumi_fid_ep* ep = (sumi_fid_ep*) ep_;
  FabricTransport* tport = (FabricTransport*) ep->domain->fabric->tport;

  const struct fi_collective_addr* caddr = (const struct fi_collective_addr*) addr;
  const SumiAvSet* set = (const SumiAvSet*) caddr->set;
  if (!set) return -FI_EINVAL;

  // Sorted, de-duplicated member ranks define the sub-communicator rank order.
  std::vector<int> members = set->ranks;
  std::sort(members.begin(), members.end());
  members.erase(std::unique(members.begin(), members.end()), members.end());
  if (members.empty()) return -FI_EINVAL;

  int my_global = tport->rank();
  auto it = std::find(members.begin(), members.end(), my_global);
  if (it == members.end()) return -FI_EINVAL;   // only members may join
  int my_comm_rank = (int) (it - members.begin());

  Communicator* comm = new MapCommunicator(my_comm_rank, std::move(members));

  struct fid_mc* m = (struct fid_mc*) calloc(1, sizeof(struct fid_mc));
  m->fid.fclass = FI_CLASS_MC;
  m->fid.context = context;
  m->fid.ops = &sumi_mc_fi_ops;
  m->fi_addr = (fi_addr_t) (uintptr_t) comm;   // decoded by collComm()
  *mc = m;
  return FI_SUCCESS;
}
