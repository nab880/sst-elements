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
 *   - Collectives run over the world communicator (engine global domain). The
 *     coll_addr from fi_join_collective is ignored; subgroups are not modeled.
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
#include <mercury/components/operating_system.h>

using SST::Iris::sumi::CollectiveEngine;
using SST::Iris::sumi::Message;
using SST::Iris::sumi::reduce_fxn;
using SST::Iris::sumi::ReduceOp;
using SST::Iris::sumi::Add;
using SST::Iris::sumi::Prod;
using SST::Iris::sumi::Min;
using SST::Iris::sumi::Max;

namespace {

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
                                 fi_addr_t /*coll_addr*/, enum fi_datatype datatype,
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
                    Message::default_cq, /*comm=*/nullptr);
  engine->blockUntilNext(Message::default_cq);

  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_barrier(struct fid_ep *ep_, fi_addr_t /*coll_addr*/, void *context)
{
  sumi_fid_ep* ep = (sumi_fid_ep*) ep_;
  FabricTransport* tport = (FabricTransport*) ep->domain->fabric->tport;
  CollectiveEngine* engine = tport->engine();
  if (!engine) return -FI_EOPNOTSUPP;

  int tag = ep->coll_tag++;
  engine->barrier(tag, Message::default_cq, /*comm=*/nullptr);
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
                                 void * /*desc*/, fi_addr_t /*coll_addr*/,
                                 fi_addr_t root_addr, enum fi_datatype datatype,
                                 uint64_t /*flags*/, void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->bcast((int) root_addr, buf, (int) count, type_size, tag,
                Message::default_cq, /*comm=*/nullptr);
  engine->blockUntilNext(Message::default_cq);
  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_reduce(struct fid_ep *ep_, const void *buf, size_t count,
                              void * /*desc*/, void *result, void * /*result_desc*/,
                              fi_addr_t /*coll_addr*/, fi_addr_t root_addr,
                              enum fi_datatype datatype, enum fi_op op,
                              uint64_t /*flags*/, void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  reduce_fxn fxn = selectFxn(op, datatype);
  if (!type_size || !fxn) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->reduce((int) root_addr, result, const_cast<void*>(buf), (int) count,
                 type_size, tag, fxn, Message::default_cq, /*comm=*/nullptr);
  engine->blockUntilNext(Message::default_cq);
  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_allgather(struct fid_ep *ep_, const void *buf, size_t count,
                                 void * /*desc*/, void *result, void * /*result_desc*/,
                                 fi_addr_t /*coll_addr*/, enum fi_datatype datatype,
                                 uint64_t /*flags*/, void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->allgather(result, const_cast<void*>(buf), (int) count, type_size, tag,
                    Message::default_cq, /*comm=*/nullptr);
  engine->blockUntilNext(Message::default_cq);
  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_gather(struct fid_ep *ep_, const void *buf, size_t count,
                              void * /*desc*/, void *result, void * /*result_desc*/,
                              fi_addr_t /*coll_addr*/, fi_addr_t root_addr,
                              enum fi_datatype datatype, uint64_t /*flags*/,
                              void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->gather((int) root_addr, result, const_cast<void*>(buf), (int) count,
                 type_size, tag, Message::default_cq, /*comm=*/nullptr);
  engine->blockUntilNext(Message::default_cq);
  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_scatter(struct fid_ep *ep_, const void *buf, size_t count,
                               void * /*desc*/, void *result, void * /*result_desc*/,
                               fi_addr_t /*coll_addr*/, fi_addr_t root_addr,
                               enum fi_datatype datatype, uint64_t /*flags*/,
                               void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->scatter((int) root_addr, result, const_cast<void*>(buf), (int) count,
                  type_size, tag, Message::default_cq, /*comm=*/nullptr);
  engine->blockUntilNext(Message::default_cq);
  deliverCompletion(ep, context);
  return FI_SUCCESS;
}

static ssize_t sumi_ep_reduce_scatter(struct fid_ep *ep_, const void *buf, size_t count,
                                      void * /*desc*/, void *result, void * /*result_desc*/,
                                      fi_addr_t /*coll_addr*/, enum fi_datatype datatype,
                                      enum fi_op op, uint64_t /*flags*/, void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  reduce_fxn fxn = selectFxn(op, datatype);
  if (!type_size || !fxn) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  engine->reduceScatter(result, const_cast<void*>(buf), (int) count, type_size,
                        tag, fxn, Message::default_cq, /*comm=*/nullptr);
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
