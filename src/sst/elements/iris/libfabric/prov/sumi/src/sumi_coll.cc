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
 * COMPLETION MODEL (non-blocking):
 *   - coll_addr selects the group: FI_ADDR_UNSPEC = the world communicator
 *     (engine global domain); otherwise it is a sub-communicator built by
 *     fi_join_collective (see sumi_cm_join / sumi_av_set below).
 *   - The op is non-blocking: fi_<coll> issues on a dedicated collective-
 *     progress CQ and returns immediately. The simulator delivers every
 *     collective work message to that CQ, so the engine progresses the
 *     collective as the client drains any CQ; on completion the matching OFI
 *     completion is posted to the endpoint's send CQ. See the "Non-blocking
 *     completion" section below for the cross-rank CQ-id contract.
 *   - Reductions map the arithmetic ops (FI_SUM/PROD/MIN/MAX) over every
 *     integer/floating datatype and the bitwise/logical ops (FI_BOR/BAND/BXOR/
 *     LOR/LAND/LXOR) over the integer datatypes; complex types and the atomic
 *     read/write/cswap ops return -FI_EOPNOTSUPP.
 */

#include "sumi_prov.h"
#include "sumi_ep.h"

#include <sumi_fabric.hpp>
#include <sumi/transport.h>
#include <sumi/comm_functions.h>
#include <sumi/communicator.h>
#include <sumi/collective.h>
#include <sumi/collective_message.h>
#include <mercury/components/operating_system.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

using SST::Iris::sumi::CollectiveEngine;
using SST::Iris::sumi::Collective;
using SST::Iris::sumi::CollectiveDoneMessage;
using SST::Iris::sumi::Communicator;
using SST::Iris::sumi::MapCommunicator;
using SST::Iris::sumi::Message;
using SST::Iris::sumi::reduce_fxn;
using SST::Iris::sumi::ReduceOp;
using SST::Iris::sumi::Add;
using SST::Iris::sumi::Prod;
using SST::Iris::sumi::Min;
using SST::Iris::sumi::Max;
using SST::Iris::sumi::BOr;
using SST::Iris::sumi::BAnd;
using SST::Iris::sumi::BXOr;
using SST::Iris::sumi::Or;    // logical OR
using SST::Iris::sumi::And;   // logical AND
using SST::Iris::sumi::LXOr;  // logical XOR

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
    case FI_INT8:
    case FI_UINT8:       return 1;
    case FI_INT16:
    case FI_UINT16:      return 2;
    case FI_INT32:
    case FI_UINT32:
    case FI_FLOAT:       return 4;
    case FI_INT64:
    case FI_UINT64:
    case FI_DOUBLE:      return 8;
    case FI_LONG_DOUBLE: return (int) sizeof(long double);
    default:             return 0;
  }
}

// Arithmetic ops (SUM/PROD/MIN/MAX): valid for every integer and floating type.
template <template <typename> class Op>
reduce_fxn fxnForType(enum fi_datatype dt)
{
  switch (dt) {
    case FI_INT8:        return &ReduceOp<Op, int8_t>::op;
    case FI_UINT8:       return &ReduceOp<Op, uint8_t>::op;
    case FI_INT16:       return &ReduceOp<Op, int16_t>::op;
    case FI_UINT16:      return &ReduceOp<Op, uint16_t>::op;
    case FI_INT32:       return &ReduceOp<Op, int32_t>::op;
    case FI_UINT32:      return &ReduceOp<Op, uint32_t>::op;
    case FI_INT64:       return &ReduceOp<Op, int64_t>::op;
    case FI_UINT64:      return &ReduceOp<Op, uint64_t>::op;
    case FI_FLOAT:       return &ReduceOp<Op, float>::op;
    case FI_DOUBLE:      return &ReduceOp<Op, double>::op;
    case FI_LONG_DOUBLE: return &ReduceOp<Op, long double>::op;
    default:             return nullptr;
  }
}

// Bitwise / logical ops (BOR/BAND/BXOR/LOR/LAND/LXOR): only meaningful for the
// integer datatypes. Instantiated separately from fxnForType so the bitwise
// operators (|,&,^) are never instantiated for float/double/long double (which
// would not compile), and float inputs are rejected with nullptr -> EOPNOTSUPP.
template <template <typename> class Op>
reduce_fxn fxnForIntType(enum fi_datatype dt)
{
  switch (dt) {
    case FI_INT8:   return &ReduceOp<Op, int8_t>::op;
    case FI_UINT8:  return &ReduceOp<Op, uint8_t>::op;
    case FI_INT16:  return &ReduceOp<Op, int16_t>::op;
    case FI_UINT16: return &ReduceOp<Op, uint16_t>::op;
    case FI_INT32:  return &ReduceOp<Op, int32_t>::op;
    case FI_UINT32: return &ReduceOp<Op, uint32_t>::op;
    case FI_INT64:  return &ReduceOp<Op, int64_t>::op;
    case FI_UINT64: return &ReduceOp<Op, uint64_t>::op;
    default:        return nullptr;
  }
}

// Map (fi_op, fi_datatype) to the engine's reduce function. Arithmetic ops
// accept any datatype; bitwise/logical ops accept integer datatypes only.
reduce_fxn selectFxn(enum fi_op op, enum fi_datatype dt)
{
  switch (op) {
    case FI_SUM:  return fxnForType<Add>(dt);
    case FI_PROD: return fxnForType<Prod>(dt);
    case FI_MIN:  return fxnForType<Min>(dt);
    case FI_MAX:  return fxnForType<Max>(dt);
    case FI_BOR:  return fxnForIntType<BOr>(dt);
    case FI_BAND: return fxnForIntType<BAnd>(dt);
    case FI_BXOR: return fxnForIntType<BXOr>(dt);
    case FI_LOR:  return fxnForIntType<Or>(dt);
    case FI_LAND: return fxnForIntType<And>(dt);
    case FI_LXOR: return fxnForIntType<LXOr>(dt);
    default:      return nullptr;
  }
}

// ---------------------------------------------------------------------------
// Non-blocking completion.
//
// fi_<coll> issues on a dedicated per-transport "collective progress" CQ and
// returns immediately -- it does NOT blockUntilNext(). Collective work messages
// accumulate on that CQ (a DefaultProgressQueue, so the simulator only *stores*
// them -- crucially it does not run collective compute in the raw NIC event
// context, where there is no current application thread). Progress is instead
// driven from the CQ-read path (sumi_progress_collectives, called by
// fi_cq_read/sread) which runs on the application thread: it pulls the stored
// messages, feeds CollectiveEngine::incoming(), and on completion posts the
// matching OFI completion to the endpoint's send CQ. This is the standard
// libfabric model (fi_cq_read progresses the provider) and replaces spike
// simplification #1 (blockUntilNext inside the op + synthesized completion).
//
// Cross-rank contract: collective work messages carry the sender's cq_id as
// their recvCQ (collective_actor.cc), so every rank MUST use the SAME integer
// for the progress CQ. All ranks run identical SPMD code and allocate CQs in
// lockstep (one fi_cq_open, then this progress CQ on the first collective), so
// the id agrees across ranks -- the same lockstep assumption the monotonic
// collective tag already relies on.

// One pending collective awaiting completion. Keyed by tag (ep->coll_tag is
// monotonic, so unique per collective instance regardless of type).
struct PendingColl {
  int send_cq_id;        // OFI CQ the completion is posted to
  void* context;         // user op_context echoed back
  uint64_t byte_length;  // bytes reported in the completion entry
  int qos;
  Communicator* comm;    // sub-communicator this runs on (nullptr = world); holds an in-flight ref
};

// Per-transport (== per-rank) collective state, keyed by the transport pointer
// so distinct ranks sharing one process never collide. Globals are safe here:
// the simulator schedules ranks cooperatively (one green thread at a time) and
// none of these operations yield.
struct CollState {
  int progress_cq_id = -1;
  std::map<int,PendingColl> pending;   // tag -> record
};

std::map<FabricTransport*,CollState> g_coll_state;

// Deferred-close bookkeeping for fi_join_collective sub-communicators. A running
// collective's engine holds the Communicator* (encoded in the mc's fi_addr), so
// fi_close(mc) must not free it while work is in flight. g_comm_inflight counts
// the outstanding collectives per Communicator; if mc_close is called while that
// count is > 0 the Communicator is parked in g_comm_close_pending and the last
// completion (releaseCommRef) performs the delete. The world communicator
// (nullptr) is never an mc and is never tracked here.
std::map<Communicator*,int> g_comm_inflight;
std::set<Communicator*> g_comm_close_pending;

// Drop one in-flight reference to `comm`; if it was closed while busy and this
// was the final reference, complete the deferred delete now.
void releaseCommRef(Communicator* comm)
{
  auto it = g_comm_inflight.find(comm);
  if (it == g_comm_inflight.end()) return;
  if (--it->second > 0) return;
  g_comm_inflight.erase(it);
  if (g_comm_close_pending.erase(comm))
    delete comm;
}

// Deliver one OFI completion by looping a short message back to ourselves,
// delivered to the send CQ (remote_cq = send_cq_id). We deliberately do NOT
// request a local ack (local_cq = no_ack): Message::cloneInjectionAck() slices
// a FabricMessage down to its base, so an ack could not carry the FabricMessage
// fields (context/flags) the CQ reader expects. An smsg is not a posted_send,
// so RecvQueue::incoming routes it straight to the CQ progress queue -- intact
// derived type -- and the client's fi_cq_read observes it.
void postCompletion(FabricTransport* tport, const PendingColl& p)
{
  if (p.send_cq_id < 0) return;
  tport->smsgSend<FabricMessage>(
      /*remote_proc*/ tport->rank(), /*byte_length*/ p.byte_length, /*buffer*/ nullptr,
      /*local_cq*/ Message::no_ack, /*remote_cq*/ p.send_cq_id,
      Message::pt2pt, /*qos*/ p.qos,
      FabricMessage::no_tag, FI_COLLECTIVE | FI_SEND, FabricMessage::no_imm_data,
      p.context);
}

// Retire a finished collective: post its completion and drop the pending record.
void completePending(FabricTransport* tport, CollState& st, int tag)
{
  auto it = st.pending.find(tag);
  if (it == st.pending.end()) return;   // e.g. internal/system-tag collective
  postCompletion(tport, it->second);
  Communicator* comm = it->second.comm;
  st.pending.erase(it);
  if (comm) releaseCommRef(comm);   // may complete a deferred mc_close
}

// True if any outstanding collective targets `cq_id`.
bool anyPendingFor(CollState& st, int cq_id)
{
  for (auto& kv : st.pending)
    if (kv.second.send_cq_id == cq_id) return true;
  return false;
}

// Lazily allocate the per-transport collective-progress CQ (see cross-rank
// contract above) and return its id. It is a DefaultProgressQueue CQ so work
// messages are only stored on arrival; sumi_progress_collectives drains them.
int collectiveCqId(FabricTransport* tport)
{
  CollState& st = g_coll_state[tport];
  if (st.progress_cq_id < 0){
    st.progress_cq_id = tport->allocateDefaultCq();
  }
  return st.progress_cq_id;
}

// Issue a collective non-blocking. Register the pending completion, then invoke
// `issue(engine, cq_id)` (which calls the engine op). If the engine op returns a
// CollectiveDoneMessage synchronously (single-rank / donothing subgroup), the
// completion is delivered right away (still on the application thread); else it
// fires later from sumi_progress_collectives. Always returns FI_SUCCESS.
template <class Issue>
ssize_t issueCollective(sumi_fid_ep* ep, int tag, uint64_t byte_length,
                        Communicator* comm, void* context, Issue&& issue)
{
  FabricTransport* tport = (FabricTransport*) ep->domain->fabric->tport;
  int cq = collectiveCqId(tport);
  CollState& st = g_coll_state[tport];
  // The completion is posted to, and progress is driven from, the CQ the app
  // reads. Prefer the send CQ (collective completions are FI_SEND); fall back to
  // the recv CQ so an endpoint bound with only a recv CQ can still make and
  // observe collective progress. Keying -1 (no CQ at all) would never match a
  // real CQ id in sumi_progress_collectives, stranding the collective.
  struct sumi_fid_cq* ccq = ep->send_cq ? ep->send_cq : ep->recv_cq;
  st.pending[tag] = PendingColl{ ccq ? ccq->id : -1,
                                 context, byte_length, ep->qos, comm };
  if (comm) ++g_comm_inflight[comm];   // keep the sub-communicator alive past mc_close
  CollectiveDoneMessage* dmsg = issue(tport->engine(), cq);
  if (dmsg){
    completePending(tport, st, tag);
    delete dmsg;   // caller owns the done message (matches mask-mpi's convention)
  }
  return FI_SUCCESS;
}

} // namespace

// Drive outstanding non-blocking collectives from the CQ-read path (application
// thread). Called by fi_cq_read/sread (sumi_cq.cc) for the CQ `cq_id` being
// read. If a pending collective targets `cq_id`, pull collective work messages
// from the progress CQ, feed the engine, and post completions -- blocking on the
// progress CQ (which pumps the simulator, delivering more work messages) until
// `cq_id`'s collective(s) complete. When `blocking` is false, only what is
// already available is processed. A no-op when nothing targets `cq_id`, so it is
// safe to call at the top of every CQ read.
void sumi_progress_collectives(FabricTransport* tport, int cq_id,
                               bool blocking, double timeout)
{
  CollState& st = g_coll_state[tport];
  if (st.progress_cq_id < 0) return;
  if (!anyPendingFor(st, cq_id)) return;
  while (anyPendingFor(st, cq_id)){
    Message* msg = tport->poll(blocking, st.progress_cq_id, timeout);
    if (!msg) return;   // non-blocking exhaustion or timeout
    CollectiveDoneMessage* dmsg = tport->engine()->incoming(msg);
    if (dmsg){
      completePending(tport, st, dmsg->tag());
      delete dmsg;
    }
  }
}

// The engine + registry live behind ep->domain->fabric->tport; every op grabs
// them the same way, then issues non-blocking via issueCollective (see above).
#define COLL_PROLOGUE(EPV)                                          \
  sumi_fid_ep* ep = (sumi_fid_ep*) (EPV);                           \
  FabricTransport* tport = (FabricTransport*) ep->domain->fabric->tport; \
  if (!tport->engine()) return -FI_EOPNOTSUPP;

static ssize_t sumi_ep_allreduce(struct fid_ep *ep_, const void *buf, size_t count,
                                 void * /*desc*/, void *result, void * /*result_desc*/,
                                 fi_addr_t coll_addr, enum fi_datatype datatype,
                                 enum fi_op op, uint64_t /*flags*/, void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  reduce_fxn fxn = selectFxn(op, datatype);
  if (!type_size || !fxn) return -FI_EOPNOTSUPP;

  // All ranks issue collectives in the same order (SPMD), so a per-endpoint
  // monotonic tag agrees across ranks for a given collective instance.
  int tag = ep->coll_tag++;
  Communicator* comm = collComm(coll_addr);

  // comm == nullptr -> engine global domain (the world communicator).
  // The registry override inside CollectiveEngine::allreduce picks the actual
  // algorithm from allreduce_alg / SUMI_ALLREDUCE_ALG.
  return issueCollective(ep, tag, (uint64_t) count * type_size, comm, context,
    [&](CollectiveEngine* engine, int cq){
      return engine->allreduce(result, const_cast<void*>(buf), (int) count,
                               type_size, tag, fxn, cq, comm);
    });
}

static ssize_t sumi_ep_barrier(struct fid_ep *ep_, fi_addr_t coll_addr, void *context)
{
  COLL_PROLOGUE(ep_);
  int tag = ep->coll_tag++;
  Communicator* comm = collComm(coll_addr);
  return issueCollective(ep, tag, /*byte_length*/ 0, comm, context,
    [&](CollectiveEngine* engine, int cq){
      return engine->barrier(tag, cq, comm);
    });
}

// `root_addr` is interpreted as a raw rank index: without fi_join_collective
// there is no AV mapping, so the client passes the root's rank directly (spike).
static ssize_t sumi_ep_broadcast(struct fid_ep *ep_, void *buf, size_t count,
                                 void * /*desc*/, fi_addr_t coll_addr,
                                 fi_addr_t root_addr, enum fi_datatype datatype,
                                 uint64_t /*flags*/, void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  Communicator* comm = collComm(coll_addr);
  return issueCollective(ep, tag, (uint64_t) count * type_size, comm, context,
    [&](CollectiveEngine* engine, int cq){
      return engine->bcast((int) root_addr, buf, (int) count, type_size, tag, cq, comm);
    });
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
  Communicator* comm = collComm(coll_addr);
  return issueCollective(ep, tag, (uint64_t) count * type_size, comm, context,
    [&](CollectiveEngine* engine, int cq){
      return engine->reduce((int) root_addr, result, const_cast<void*>(buf),
                            (int) count, type_size, tag, fxn, cq, comm);
    });
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
  Communicator* comm = collComm(coll_addr);
  return issueCollective(ep, tag, (uint64_t) count * type_size, comm, context,
    [&](CollectiveEngine* engine, int cq){
      return engine->allgather(result, const_cast<void*>(buf), (int) count,
                               type_size, tag, cq, comm);
    });
}

static ssize_t sumi_ep_alltoall(struct fid_ep *ep_, const void *buf, size_t count,
                                void * /*desc*/, void *result, void * /*result_desc*/,
                                fi_addr_t coll_addr, enum fi_datatype datatype,
                                uint64_t /*flags*/, void *context)
{
  COLL_PROLOGUE(ep_);
  int type_size = datatypeSize(datatype);
  if (!type_size) return -FI_EOPNOTSUPP;
  int tag = ep->coll_tag++;
  Communicator* comm = collComm(coll_addr);
  // count is the per-peer element count (each rank sends `count` elems to every
  // rank); the engine's Bruck/Direct all-to-all actor selects the algorithm.
  return issueCollective(ep, tag, (uint64_t) count * type_size, comm, context,
    [&](CollectiveEngine* engine, int cq){
      return engine->alltoall(result, const_cast<void*>(buf), (int) count,
                              type_size, tag, cq, comm);
    });
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
  Communicator* comm = collComm(coll_addr);
  return issueCollective(ep, tag, (uint64_t) count * type_size, comm, context,
    [&](CollectiveEngine* engine, int cq){
      return engine->gather((int) root_addr, result, const_cast<void*>(buf),
                            (int) count, type_size, tag, cq, comm);
    });
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
  Communicator* comm = collComm(coll_addr);
  return issueCollective(ep, tag, (uint64_t) count * type_size, comm, context,
    [&](CollectiveEngine* engine, int cq){
      return engine->scatter((int) root_addr, result, const_cast<void*>(buf),
                             (int) count, type_size, tag, cq, comm);
    });
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
  Communicator* comm = collComm(coll_addr);
  return issueCollective(ep, tag, (uint64_t) count * type_size, comm, context,
    [&](CollectiveEngine* engine, int cq){
      return engine->reduceScatter(result, const_cast<void*>(buf), (int) count,
                                   type_size, tag, fxn, cq, comm);
    });
}

struct fi_ops_collective sumi_ep_collective_ops = {
  .size = sizeof(struct fi_ops_collective),
  .barrier = sumi_ep_barrier,
  .broadcast = sumi_ep_broadcast,
  .alltoall = sumi_ep_alltoall,
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
  // dst := dst ∪ src, without introducing duplicate members.
  for (int r : src)
    if (std::find(dst.begin(), dst.end(), r) == dst.end())
      dst.push_back(r);
  return FI_SUCCESS;
}

int avset_intersect(struct fid_av_set* d, const struct fid_av_set* s) {
  auto& dst = ((SumiAvSet*) d)->ranks;
  const auto& src = ((const SumiAvSet*) s)->ranks;
  // dst := dst ∩ src (keep only members also present in src).
  std::vector<int> keep;
  for (int r : dst)
    if (std::find(src.begin(), src.end(), r) != src.end())
      keep.push_back(r);
  dst.swap(keep);
  return FI_SUCCESS;
}

int avset_diff(struct fid_av_set* d, const struct fid_av_set* s) {
  auto& dst = ((SumiAvSet*) d)->ranks;
  const auto& src = ((const SumiAvSet*) s)->ranks;
  // dst := dst \ src (drop members present in src).
  std::vector<int> keep;
  for (int r : dst)
    if (std::find(src.begin(), src.end(), r) == src.end())
      keep.push_back(r);
  dst.swap(keep);
  return FI_SUCCESS;
}

int avset_addr(struct fid_av_set*, fi_addr_t* coll_addr) {
  // This provider cannot synthesize a usable coll_addr from an av_set: the
  // sub-communicator only exists after fi_join_collective, whose mc encodes the
  // group (fi_mc_addr(mc) is the coll_addr to use). There is no fi_addr value we
  // could return here that collComm() would decode as "this subgroup" -- any
  // in-range value casts to a Communicator* and 0/FI_ADDR_UNSPEC both decode as
  // the world communicator. So fail loudly rather than silently route to world.
  if (coll_addr) *coll_addr = FI_ADDR_NOTAVAIL;
  return -FI_ENOSYS;
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
  Communicator* comm = reinterpret_cast<Communicator*>((uintptr_t) m->fi_addr);
  // If collectives are still in flight on this sub-communicator, the engine
  // holds `comm`; deleting now would be a use-after-free. Park it and let the
  // final completion (releaseCommRef) delete it. Otherwise delete immediately.
  auto it = g_comm_inflight.find(comm);
  if (comm && it != g_comm_inflight.end() && it->second > 0)
    g_comm_close_pending.insert(comm);
  else
    delete comm;
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

int sumi_av_set(struct fid_av* /*av*/, struct fi_av_set_attr* attr,
                struct fid_av_set** av_set, void* context) {
  SumiAvSet* set = new SumiAvSet();
  set->av_set_fid.fid.fclass = FI_CLASS_AV_SET;
  set->av_set_fid.fid.context = context;
  set->av_set_fid.fid.ops = &sumi_av_set_fi_ops;
  set->av_set_fid.ops = &sumi_av_set_ops;

  // Range form: attr with count>0 pre-populates membership as the arithmetic
  // sequence start_addr, start_addr+stride, ... (count entries). start_addr and
  // stride are interpreted as raw global rank indices (spike: no AV lookup), so
  // e.g. {start=0, stride=2, count=n/2} is the even-rank subgroup. When count is
  // 0 the set starts empty and the client fills it via fi_av_set_insert.
  if (attr && attr->count > 0) {
    uint64_t stride = attr->stride ? attr->stride : 1;
    for (size_t i = 0; i < attr->count; ++i)
      set->ranks.push_back((int) (attr->start_addr + (fi_addr_t) i * stride));
  }

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

  // Signal readiness through the endpoint's event queue: a client following the
  // libfabric contract waits for FI_JOIN_COMPLETE on the EQ before using the mc.
  // The MapCommunicator is built synchronously, so the event is posted now (a
  // fully deferred join would post it after a group-formation barrier). If no EQ
  // is bound, the mc is simply usable immediately (backward-compatible).
  if (ep->eq)
    sumi_eq_post(ep->eq, FI_JOIN_COMPLETE, &m->fid, context, /*data*/ 0);

  return FI_SUCCESS;
}
