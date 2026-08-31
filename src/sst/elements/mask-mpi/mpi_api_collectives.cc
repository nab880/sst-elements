/**
Copyright 2009-2026 National Technology and Engineering Solutions of Sandia,
LLC (NTESS).  Under the terms of Contract DE-NA-0003525, the U.S. Government
retains certain rights in this software.

Sandia National Laboratories is a multimission laboratory managed and operated
by National Technology and Engineering Solutions of Sandia, LLC., a wholly
owned subsidiary of Honeywell International, Inc., for the U.S. Department of
Energy's National Nuclear Security Administration under contract DE-NA0003525.

Copyright (c) 2009-2026, NTESS

All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.

    * Neither the name of the copyright holder nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Questions? Contact sst-macro-help@sandia.gov
*/

#include <mpi_api.h>
#include <mpi_queue/mpi_queue.h>
//#include <sumi-mpi/otf2_output_stat.h>
#include <mercury/components/operating_system.h>
#include <mercury/operating_system/process/app.h>
#include <mercury/operating_system/process/thread.h>
#//include <mercury/operating_system/process/ftq_scope.h>

#include <limits>
#include <optional>

//#define do_coll(coll, fxn, ...) \
//  StartMPICall(fxn); \
//  auto op = start##coll(#fxn, __VA_ARGS__); \
//  waitCollective(std::move(op)); \
//  FinishMPICall(fxn);

#define do_coll(coll, fxn, ...) \
  auto op = start##coll(#fxn, __VA_ARGS__); \
  waitCollective(std::move(op));

//#define start_coll(coll, fxn, ...) \
//  StartMPICall(fxn); \
//  auto op = start##coll(#fxn, __VA_ARGS__); \
//  addImmediateCollective(std::move(op), req); \
//  FinishMPICall(fxn)

#define start_coll(coll, fxn, ...) \
  auto op = start##coll(#fxn, __VA_ARGS__); \
  addImmediateCollective(std::move(op), req);

namespace SST::MASKMPI {

namespace {

using SST::Collective::CollectiveDatatype;
using SST::Collective::CollectiveOperation;
using SST::Collective::CollectiveSignatureV1;

struct MappedCollectiveSignature
{
  CollectiveSignatureV1 signature;
  uint64_t payload_bytes = 0;
};

std::optional<CollectiveOperation>
mapCollectiveOperation(MPI_Op operation)
{
  switch (operation) {
    case MPI_SUM: return CollectiveOperation::Sum;
    case MPI_MIN: return CollectiveOperation::Min;
    case MPI_MAX: return CollectiveOperation::Max;
    default: return std::nullopt;
  }
}

std::optional<CollectiveDatatype>
mapSignedCollectiveDatatype(int packed_width)
{
  if (packed_width == 4) return CollectiveDatatype::I32;
  if (packed_width == 8) return CollectiveDatatype::I64;
  return std::nullopt;
}

std::optional<CollectiveDatatype>
mapUnsignedCollectiveDatatype(int packed_width)
{
  if (packed_width == 4) return CollectiveDatatype::U32;
  if (packed_width == 8) return CollectiveDatatype::U64;
  return std::nullopt;
}

std::optional<CollectiveDatatype>
mapFloatingCollectiveDatatype(int packed_width)
{
  if (packed_width == 4) return CollectiveDatatype::F32;
  if (packed_width == 8) return CollectiveDatatype::F64;
  return std::nullopt;
}

std::optional<CollectiveDatatype>
mapCollectiveDatatype(MPI_Datatype datatype, int packed_width)
{
  switch (datatype) {
    case MPI_INT:
    case MPI_INTEGER:
    case MPI_INT32_T:
    case MPI_INTEGER4:
    case MPI_LONG:
    case MPI_LONG_LONG_INT:
    case MPI_INT64_T:
    case MPI_INTEGER8:
      return mapSignedCollectiveDatatype(packed_width);
    case MPI_UNSIGNED:
    case MPI_UINT32_T:
    case MPI_UNSIGNED_LONG:
    case MPI_UNSIGNED_LONG_LONG:
    case MPI_UINT64_T:
      return mapUnsignedCollectiveDatatype(packed_width);
    case MPI_FLOAT:
    case MPI_REAL:
    case MPI_REAL4:
    case MPI_DOUBLE:
    case MPI_DOUBLE_PRECISION:
    case MPI_REAL8:
      return mapFloatingCollectiveDatatype(packed_width);
    default:
      return std::nullopt;
  }
}

std::optional<MappedCollectiveSignature>
mapCollectiveSignature(MPI_Op operation, MPI_Datatype datatype,
                       int element_count, int packed_width)
{
  const auto mapped_operation = mapCollectiveOperation(operation);
  const auto mapped_datatype = mapCollectiveDatatype(datatype, packed_width);
  if (!mapped_operation || !mapped_datatype || element_count <= 0 || packed_width <= 0) {
    return std::nullopt;
  }

  const uint64_t expected_width = SST::Collective::collectiveDatatypeBytes(*mapped_datatype);
  if (expected_width == 0 || static_cast<uint64_t>(packed_width) != expected_width) {
    return std::nullopt;
  }

  CollectiveSignatureV1 signature {
      *mapped_operation, *mapped_datatype, static_cast<uint64_t>(element_count)};
  const auto payload_bytes = signature.payloadBytes();
  if (!payload_bytes || *payload_bytes > std::numeric_limits<size_t>::max()) {
    return std::nullopt;
  }
  return MappedCollectiveSignature {signature, *payload_bytes};
}

} // namespace

MpiRequest*
MpiApi::addImmediateCollective(CollectiveOpBase::ptr&& op)
{
  MpiRequest* reqPtr = MpiRequest::construct(MpiRequest::Collective);
  op->comm->addRequest(op->tag, reqPtr);
  if (op->complete){
    finishCollective(op.get());
    reqPtr->complete();
  }
  reqPtr->setCollective(std::move(op));
  return reqPtr;
}

void
MpiApi::addImmediateCollective(CollectiveOpBase::ptr&& op, MPI_Request* req)
{
  auto* reqPtr = addImmediateCollective(std::move(op));
  addRequestPtr(reqPtr, req);
}

void
MpiApi::startMpiCollective(Iris::sumi::Collective::type_t ty,
                           const void *sendbuf, void *recvbuf,
                           MPI_Datatype sendtype, MPI_Datatype recvtype,
                           CollectiveOpBase* op)
{
  op->ty = ty;
  op->sendbuf = const_cast<void*>(sendbuf);
  op->recvbuf = recvbuf;

  if (sendbuf == MPI_IN_PLACE){
    if (recvbuf){
      MpiType* type = typeFromId(recvtype);
      int offset;
      switch(ty){
        case Iris::sumi::Collective::gather:
        case Iris::sumi::Collective::allgather:
          offset = type->extent() * op->recvcnt * op->comm->rank();
          break;
        default:
          offset = 0;
          break;
      }
      op->sendbuf = ((char*)recvbuf) + offset;
    }
    op->sendcnt = op->recvcnt;
    sendtype = recvtype;
  }

  if (sendtype != recvtype){
    if (sendtype == MPI_DATATYPE_NULL || sendtype == MPI_NULL){
      sendtype = recvtype;
      op->sendcnt = op->recvcnt;
      if (sendbuf != MPI_IN_PLACE) op->sendbuf = nullptr;
    } else if (recvtype == MPI_DATATYPE_NULL || recvtype == MPI_NULL){
      recvtype = sendtype;
      op->recvcnt = op->sendcnt;
      if (recvbuf != MPI_IN_PLACE) op->recvbuf = nullptr;
    }
  }

  op->tmp_sendbuf = op->sendbuf;
  op->tmp_recvbuf = op->recvbuf;

  op->sendtype = typeFromId(sendtype);
  op->recvtype = typeFromId(recvtype);
  op->packed_recv = false;
  op->packed_send = false;

  if (op->sendbuf && !op->sendtype->contiguous()){
    void* newbuf = allocateTempPackBuffer(op->sendcnt, op->sendtype);
    op->sendtype->packSend(op->sendbuf, newbuf, op->sendcnt);
    op->tmp_sendbuf = newbuf;
    op->packed_send = true;
  } else {
    op->tmp_sendbuf = op->sendbuf;
  }

  if (op->recvbuf && !op->recvtype->contiguous()){
    void* newbuf = allocateTempPackBuffer(op->recvcnt, op->recvtype);
    op->tmp_recvbuf = newbuf;
    op->packed_recv = true;
  } else {
    op->tmp_recvbuf = recvbuf;
  }


}

void*
MpiApi::allocateTempPackBuffer(int count, MpiType* type)
{
  char* newbuf = new char[type->packed_size()*count];
  return newbuf;
}

void
MpiApi::freeTempPackBuffer(void* srcbuf)
{
  char* buf = (char*) srcbuf;
  delete[] buf;
}

void
MpiApi::finishCollectiveOp(CollectiveOpBase* op_)
{
  CollectiveOp* op = static_cast<CollectiveOp*>(op_);
//  mpi_api_debug(sprockit::dbg::mpi_collective,
//                "finishing op on tag %d for collective %s: packed=(%d,%d)",
//                op->tag, Collective::tostr(op->ty),
//                op->packed_send, op->packed_recv);

  if (op->packed_recv){
    op->recvtype->unpack_recv(op->tmp_recvbuf, op->recvbuf, op->recvcnt);
    freeTempPackBuffer(op->tmp_recvbuf);
  }
  if (op->packed_send){
    freeTempPackBuffer(op->tmp_sendbuf);
  }
}

void
MpiApi::finishCollective(CollectiveOpBase* op)
{
  switch(op->ty){
    case Iris::sumi::Collective::donothing:
      sst_hg_abort_printf("do nothing collective should not call finishCollective");
    case Iris::sumi::Collective::reduce:
    case Iris::sumi::Collective::alltoall:
    case Iris::sumi::Collective::gather:
    case Iris::sumi::Collective::scatter:
    case Iris::sumi::Collective::allreduce:
    case Iris::sumi::Collective::scan:
    case Iris::sumi::Collective::allgather:
    case Iris::sumi::Collective::barrier:
    case Iris::sumi::Collective::reduce_scatter:
    case Iris::sumi::Collective::bcast:
      finishCollectiveOp(op);
      break;
    case Iris::sumi::Collective::alltoallv:
    case Iris::sumi::Collective::gatherv:
    case Iris::sumi::Collective::scatterv:
    case Iris::sumi::Collective::allgatherv:
      finishVcollectiveOp(op);
      break;
  }
}

void
MpiApi::waitCollectives(std::vector<CollectiveOpBase::ptr>&& ops)
{
  std::vector<MpiRequest*> reqs;
  for (auto&& op : ops){
    reqs.emplace_back(addImmediateCollective(std::move(op)));
  }

  for (auto* req : reqs){
    if (!req->isComplete()){
      queue_->progressLoop(req);
    }
    delete req;
  }
}

void
MpiApi::waitCollective(CollectiveOpBase::ptr&& op)
{
  bool is_comm_world = op->comm->id() == MPI_COMM_WORLD;
  MpiRequest* req = addImmediateCollective(std::move(op));
  if (!req->isComplete()){
    queue_->progressLoop(req);
  }
  if (is_comm_world) {
    crossed_comm_world_barrier_ = true;
  }
  delete req;
}

Iris::sumi::CollectiveDoneMessage*
MpiApi::startAllgather(CollectiveOp *op)
{
  return engine_->allgather(op->tmp_recvbuf, op->tmp_sendbuf,
                  op->sendcnt, op->sendtype->packed_size(), op->tag,
                  queue_->collCqId(), op->comm);
}

CollectiveOpBase::ptr
MpiApi::startAllgather(const char* name, MPI_Comm comm, int sendcount, MPI_Datatype sendtype,
                         int recvcount, MPI_Datatype recvtype, const void *sendbuf, void *recvbuf)
{
//  mpi_api_debug(sprockit::dbg::mpi | sprockit::dbg::mpi_collective,
//    "%s(%d,%s,%d,%s,%s)", name,
//    sendcount, typeStr(sendtype).c_str(),
//    recvcount, typeStr(recvtype).c_str(),
//    commStr(comm).c_str());

  auto op = CollectiveOp::create(sendcount, recvcount, getComm(comm));
  startMpiCollective(Iris::sumi::Collective::allgather, sendbuf, recvbuf, sendtype, recvtype, op.get());
  auto* msg = startAllgather(op.get());
  if (msg){
    op->complete = true;
    delete msg;
  }
  return std::move(op);
}

int
MpiApi::allgather(int sendcount, MPI_Datatype sendtype,
                   int recvcount, MPI_Datatype recvtype, MPI_Comm comm)
{
  return allgather(NULL, sendcount, sendtype, NULL, recvcount, recvtype, comm);
}

int
MpiApi::allgather(int count, MPI_Datatype type, MPI_Comm comm){
  return allgather(count, type, count, type, comm);
}


int
MpiApi::allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                   void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm)
{
#ifdef SST_HG_OTF2_ENABLED
  auto start_clock = traceClock();
#endif

  do_coll(Allgather, MPI_Allgather, comm,
          sendcount, sendtype, recvcount, recvtype,
          sendbuf, recvbuf);

#ifdef SST_HG_OTF2_ENABLED
  if (OTF2Writer_){
    OTF2Writer_->writer().mpi_allgather(start_clock, traceClock(),
            sendcount, sendtype, recvcount, recvtype, comm);
  }
#endif

  return MPI_SUCCESS;

}

int
MpiApi::iallgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                    void *recvbuf, int recvcount, MPI_Datatype recvtype,
                    MPI_Comm comm, MPI_Request *req)
{
  start_coll(Allgather, MPI_Iallgather, comm,
             sendcount, sendtype,
             recvcount, recvtype,
             sendbuf, recvbuf);
  return MPI_SUCCESS;
}

int
MpiApi::iallgather(int sendcount, MPI_Datatype sendtype,
                    int recvcount, MPI_Datatype recvtype,
                    MPI_Comm comm, MPI_Request *req)
{
  return iallgather(NULL, sendcount, sendtype, NULL, recvcount, recvtype, comm, req);
}

Iris::sumi::CollectiveDoneMessage*
MpiApi::startAlltoall(CollectiveOp* op)
{
  return engine_->alltoall(op->tmp_recvbuf, op->tmp_sendbuf, op->sendcnt,
                      op->sendtype->packed_size(), op->tag,
                      queue_->collCqId(), op->comm);
}

CollectiveOpBase::ptr
MpiApi::startAlltoall(const char* name, MPI_Comm comm, int sendcount, MPI_Datatype sendtype,
                      int recvcount, MPI_Datatype recvtype, const void *sendbuf, void *recvbuf)
{
//  mpi_api_debug(sprockit::dbg::mpi | sprockit::dbg::mpi_collective,
//    "%s(%d,%s,%d,%s,%s)", name,
//    sendcount, typeStr(sendtype).c_str(),
//    recvcount, typeStr(recvtype).c_str(),
//    commStr(comm).c_str());

  auto op = CollectiveOp::create(sendcount, recvcount, getComm(comm));
  startMpiCollective(Iris::sumi::Collective::alltoall, sendbuf, recvbuf, sendtype, recvtype, op.get());
  auto* msg = startAlltoall(op.get());
  if (msg){
    op->complete = true;
    delete msg;
  }
  return std::move(op);
}

int
MpiApi::alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 MPI_Comm comm)
{
#ifdef SST_HG_OTF2_ENABLED
  auto start_clock = traceClock();
#endif

  do_coll(Alltoall, MPI_Alltoall, comm,
         sendcount, sendtype,
         recvcount, recvtype,
         sendbuf, recvbuf);

#ifdef SST_HG_OTF2_ENABLED
  if (OTF2Writer_){
    OTF2Writer_->writer().mpi_alltoall(start_clock, traceClock(),
                           sendcount, sendtype, recvcount, recvtype, comm);
  }
#endif

  return MPI_SUCCESS;
}


int
MpiApi::alltoall(int sendcount, MPI_Datatype sendtype,
                  int recvcount, MPI_Datatype recvtype, MPI_Comm comm)
{
  return alltoall(NULL, sendcount, sendtype, NULL, recvcount, recvtype, comm);
}

int
MpiApi::ialltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  MPI_Comm comm, MPI_Request* req)
{
  start_coll(Alltoall, MPI_Ialltoall, comm,
             sendcount, sendtype,
             recvcount, recvtype,
             sendbuf, recvbuf);
  return MPI_SUCCESS;
}

int
MpiApi::ialltoall(int sendcount, MPI_Datatype sendtype,
                  int recvcount, MPI_Datatype recvtype,
                   MPI_Comm comm, MPI_Request* req)
{
  return ialltoall(NULL, sendcount, sendtype, NULL,
                   recvcount, recvtype, comm, req);
}

Iris::sumi::CollectiveDoneMessage*
MpiApi::startAllreduce(CollectiveOp* op)
{
  Iris::sumi::reduce_fxn fxn = getCollectiveFunction(op);
  return engine_->allreduce(op->tmp_recvbuf, op->tmp_sendbuf, op->sendcnt,
                       op->sendtype->packed_size(), op->tag,
                       fxn, queue_->collCqId(), op->comm);
}

CollectiveOp::ptr
MpiApi::prepareAllreduce(MpiComm* commPtr, int count, MPI_Datatype type,
                         MPI_Op mop, const void* src, void* dst)
{
  auto op = CollectiveOp::create(count, commPtr);
  if (src == MPI_IN_PLACE){
    src = dst;
  }

  op->op = mop;
  startMpiCollective(Iris::sumi::Collective::allreduce, src, dst, type, type, op.get());
  return op;
}

CollectiveOpBase::ptr
MpiApi::startAllreduce(MpiComm* commPtr, int count, MPI_Datatype type,
                       MPI_Op mop, const void* src, void* dst)
{
  auto op = prepareAllreduce(commPtr, count, type, mop, src, dst);
  auto* msg = startAllreduce(op.get());
  if (msg){
    op->complete = true;
    delete msg;
  }
  return std::move(op);
}

bool
MpiApi::bindCollectiveOffload()
{
  if (collective_endpoint_) return true;

  auto* operating_system = parent()->os();
  auto* endpoint = operating_system ? operating_system->collectiveEndpoint() : nullptr;
  auto* participant_ptr = operating_system ? operating_system->collectiveParticipant(0) : nullptr;
  if (!endpoint || !participant_ptr) return false;

  const auto& participant = *participant_ptr;
  if (!participant.valid() ||
      participant.route_kind != SST::Collective::CollectiveRouteKind::FabricTree ||
      participant.data_mode != SST::Collective::CollectiveDataMode::Functional ||
      participant.local_participant_slot != 0 || participant.local_participant_count != 1 ||
      participant.physical_endpoint_id < 0 || !participant.fabric ||
      participant.fabric->endpoint_reduce_vn != 0 ||
      participant.fabric->endpoint_result_vn != 1 ||
      participant.fabric->injection_dest_nid < 0 ||
      !worldcomm_ || participant.logical_participant_id != static_cast<uint64_t>(worldcomm_->rank()) ||
      participant.accepted_invocation_quota != 1 || participant.submission_window != 1) {
    return false;
  }

  if (!endpoint->bindParticipant(participant, *this, *this)) return false;
  collective_endpoint_ = endpoint;
  collective_participant_ = participant_ptr;
  return true;
}

void
MpiApi::clearCollectiveOffloadRequest()
{
  if (collective_request_comm_ && collective_request_ &&
      !collective_request_comm_->removeRequest(collective_request_tag_, collective_request_)) {
    sst_hg_abort_printf("Mask-MPI collective offload lost its registered native request");
  }
  collective_request_ = nullptr;
  collective_request_comm_ = nullptr;
  collective_waiter_ = nullptr;
  collective_waiting_blocked_ = false;
  collective_request_tag_ = 0;
  collective_request_invocation_ = 0;
  collective_ready_ = false;
  collective_completion_status_ = SST::Collective::CollectiveCompletionStatus::RecoverableError;
}

void
MpiApi::complete(SST::Collective::CollectiveCompletionToken&& token,
                 SST::Collective::CollectiveCompletionStatus status)
{
  if (!SST::Collective::isValid(status) || !collective_request_ || !collective_request_comm_ ||
      !collective_participant_ || !token.valid() ||
      token.adapterSlot() != collective_participant_->binding.adapter_slot ||
      token.generation() != collective_participant_->binding.generation ||
      token.nativeRequestId() != collective_request_invocation_ ||
      collective_request_comm_->getRequest(collective_request_tag_) != collective_request_ ||
      collective_request_->isComplete()) {
    sst_hg_abort_printf("Mask-MPI received an invalid or duplicate collective offload completion");
  }

  collective_completion_status_ = status;
  collective_request_->complete();
  if (collective_waiter_ && collective_waiting_blocked_) {
    parent()->os()->unblock(collective_waiter_);
  }
}

void
MpiApi::ready(const SST::Collective::AcceptedParticipantHandle& participant)
{
  if (!collective_request_ || !collective_participant_ ||
      participant.route != collective_participant_->route ||
      !(participant.binding == collective_participant_->binding)) {
    sst_hg_abort_printf("Mask-MPI received an invalid collective offload ready notification");
  }
  collective_ready_ = true;
  if (collective_waiter_ && collective_waiting_blocked_) {
    parent()->os()->unblock(collective_waiter_);
  }
}

bool
MpiApi::tryBlockingAllreduceOffload(CollectiveOp::ptr& op, MPI_Datatype type, MPI_Op mop)
{
  if (!collective_offload_enabled_) {
    return false;
  }
  auto* operating_system = parent()->os();
  if (op->comm != worldcomm_ || op->comm->id() != MPI_COMM_WORLD ||
      op->sendcnt != op->recvcnt) {
    return false;
  }
  const auto mapped_signature = mapCollectiveSignature(
      mop, type, op->sendcnt, op->sendtype->packed_size());
  if (!mapped_signature) return false;

  auto* candidate_endpoint = collective_endpoint_ != nullptr ? collective_endpoint_ :
      (operating_system != nullptr ? operating_system->collectiveEndpoint() : nullptr);
  if (candidate_endpoint != nullptr &&
      !candidate_endpoint->supportsCollective(mapped_signature->signature)) {
    return false;
  }

  if (!operating_system || operating_system->ranksPerNode() != 1 ||
      op->packed_send || op->packed_recv || op->tmp_sendbuf == nullptr ||
      op->tmp_recvbuf == nullptr) {
    sst_hg_abort_printf(
        "Mask-MPI collective offload is enabled but this rank cannot execute a supported call");
  }
  if (!bindCollectiveOffload()) {
    sst_hg_abort_printf("Mask-MPI collective offload is enabled but no valid endpoint is available");
  }
  if (collective_request_ != nullptr || op->tag < 0) {
    sst_hg_abort_printf("Mask-MPI collective offload supports one outstanding blocking call");
  }

  const uint64_t invocation_id = next_collective_invocation_++;
  if (invocation_id == 0 || next_collective_invocation_ == 0) {
    sst_hg_abort_printf("Mask-MPI collective offload invocation sequence exhausted");
  }

  auto* request = MpiRequest::construct(MpiRequest::Collective);
  collective_request_ = request;
  collective_request_comm_ = op->comm;
  collective_request_tag_ = op->tag;
  collective_request_invocation_ = invocation_id;
  collective_completion_status_ = SST::Collective::CollectiveCompletionStatus::RecoverableError;
  op->comm->addRequest(op->tag, request);

  SST::Collective::CollectivePending pending;
  pending.participant = *collective_participant_;
  pending.invocation_id = invocation_id;
  pending.signature = mapped_signature->signature;
  pending.source = {reinterpret_cast<const uint8_t*>(op->tmp_sendbuf),
                    mapped_signature->payload_bytes};
  pending.result = {reinterpret_cast<uint8_t*>(op->tmp_recvbuf),
                    mapped_signature->payload_bytes};
  pending.completion = SST::Collective::CollectiveCompletionToken(
      collective_participant_->binding.adapter_slot, invocation_id,
      collective_participant_->binding.generation);

  while (true) {
    const auto result = collective_endpoint_->trySubmitCollective(pending);
    if (result == SST::Collective::CollectiveSubmitResult::Accepted) {
      if (pending.state != SST::Collective::CollectivePendingState::Consumed || pending.completion.valid()) {
        sst_hg_abort_printf("Mask-MPI collective endpoint accepted without consuming ownership");
      }
      request->setCollective(std::move(op));
      collective_waiter_ = operating_system->activeThread();
      if (!collective_waiter_) {
        sst_hg_abort_printf("Mask-MPI collective offload has no active application thread");
      }
      while (!request->isComplete()) {
        collective_waiting_blocked_ = true;
        operating_system->block();
        collective_waiting_blocked_ = false;
      }
      collective_waiter_ = nullptr;

      if (collective_completion_status_ != SST::Collective::CollectiveCompletionStatus::Success) {
        auto retry_op = request->takeCollective();
        clearCollectiveOffloadRequest();
        delete request;
        if (!retry_op) {
          sst_hg_abort_printf("Mask-MPI collective offload lost the operation needed for recovery");
        }
        op.reset(static_cast<CollectiveOp*>(retry_op.release()));
        return false;
      }
      finishCollective(request->collectiveData());
      clearCollectiveOffloadRequest();
      delete request;
      return true;
    }

    if (!pending.readyForSubmit()) {
      sst_hg_abort_printf("Mask-MPI collective endpoint consumed ownership without accepting");
    }
    if (result == SST::Collective::CollectiveSubmitResult::Unsupported) {
      clearCollectiveOffloadRequest();
      delete request;
      return false;
    }
    if (result == SST::Collective::CollectiveSubmitResult::Invalid) {
      clearCollectiveOffloadRequest();
      delete request;
      sst_hg_abort_printf("Mask-MPI collective endpoint rejected a valid POC submission");
    }
    if (result != SST::Collective::CollectiveSubmitResult::Retry) {
      sst_hg_abort_printf("Mask-MPI collective endpoint returned an unknown status");
    }

    collective_ready_ = false;
    collective_waiter_ = operating_system->activeThread();
    if (!collective_waiter_) {
      sst_hg_abort_printf("Mask-MPI collective retry has no active application thread");
    }
    collective_endpoint_->requestCollectiveReady(
        *collective_participant_, pending.signature);
    while (!collective_ready_) {
      collective_waiting_blocked_ = true;
      operating_system->block();
      collective_waiting_blocked_ = false;
    }
    collective_waiter_ = nullptr;
  }
}


CollectiveOpBase::ptr
MpiApi::startAllreduce(const char* name, MPI_Comm comm, int count, MPI_Datatype type,
                       MPI_Op mop, const void* src, void* dst)
{
//  mpi_api_debug(sprockit::dbg::mpi | sprockit::dbg::mpi_collective,
//    "%s(%d,%s,%s)", name,
//    count, typeStr(type).c_str(),
//    commStr(comm).c_str());

  return startAllreduce(getComm(comm), count, type, mop, src, dst);
}

int
MpiApi::allreduce(const void *src, void *dst, int count,
                   MPI_Datatype type, MPI_Op mop, MPI_Comm comm)
{
#ifdef SST_HG_OTF2_ENABLED
  auto start_clock = traceClock();
#endif

  auto op = prepareAllreduce(getComm(comm), count, type, mop, src, dst);
  if (!tryBlockingAllreduceOffload(op, type, mop)) {
    auto* msg = startAllreduce(op.get());
    if (msg) {
      op->complete = true;
      delete msg;
    }
    waitCollective(std::move(op));
  }
  crossed_comm_world_barrier_ = (comm == MPI_COMM_WORLD) || crossed_comm_world_barrier_;

#ifdef SST_HG_OTF2_ENABLED
  if (OTF2Writer_){
    OTF2Writer_->writer().mpi_allreduce(start_clock, traceClock(),
                            count, type, comm);
  }
#endif

  return MPI_SUCCESS;
}

int
MpiApi::allreduce(int count, MPI_Datatype type, MPI_Op op, MPI_Comm comm)
{
  return allreduce(NULL, NULL, count, type, op, comm);
}

int
MpiApi::iallreduce(const void *src, void *dst, int count,
                   MPI_Datatype type, MPI_Op mop,
                    MPI_Comm comm, MPI_Request* req)
{
  start_coll(Allreduce, MPI_Iallreduce,
              comm, count, type, mop, src, dst);
  return MPI_SUCCESS;
}

int
MpiApi::iallreduce(int count, MPI_Datatype type, MPI_Op op,
                    MPI_Comm comm, MPI_Request* req)
{
  return iallreduce(NULL, NULL, count, type, op, comm, req);
}

Iris::sumi::CollectiveDoneMessage*
MpiApi::startBarrier(CollectiveOp* op)
{
  op->ty = Iris::sumi::Collective::barrier;
  return engine_->barrier(op->tag, queue_->collCqId(), op->comm);
}

CollectiveOpBase::ptr
MpiApi::startBarrier(const char* name, MPI_Comm comm)
{
  auto op = CollectiveOp::create(0, getComm(comm));
//  mpi_api_debug(sprockit::dbg::mpi, "%s(%s) on tag %d",
//    name, commStr(comm).c_str(), int(op->tag));
  auto* msg = startBarrier(op.get());
  if (msg){
    op->complete = true;
    delete msg;
  }
  return std::move(op);
}

int
MpiApi::barrier(MPI_Comm comm)
{
#ifdef SST_HG_OTF2_ENABLED
  auto start_clock = traceClock();
#endif

  //StartMPICall(MPI_Barrier);
  waitCollective( startBarrier("MPI_Barrier", comm) );
  //FinishMPICall(MPI_Barrier);

#ifdef SST_HG_OTF2_ENABLED
  if(OTF2Writer_) {
    OTF2Writer_->writer().mpi_barrier(start_clock, traceClock(), comm);
  }
#endif

  return MPI_SUCCESS;
}

int
MpiApi::ibarrier(MPI_Comm comm, MPI_Request *req)
{
  //StartMPICall(MPI_Ibarrier);
  addImmediateCollective(startBarrier("MPI_Ibarrier", comm), req);
  //FinishMPICall(MPI_Ibarrier);
  return MPI_SUCCESS;
}

Iris::sumi::CollectiveDoneMessage*
MpiApi::startBcast(CollectiveOp* op)
{
  void* buf = op->comm->rank() == op->root ? op->tmp_sendbuf : op->tmp_recvbuf;
  return engine_->bcast(op->root, buf, op->sendcnt,
                 op->sendtype->packed_size(), op->tag,
                 queue_->collCqId(), op->comm);
}

CollectiveOpBase::ptr
MpiApi::startBcast(const char* name, MPI_Comm comm, int count, MPI_Datatype datatype, int root, void *buffer)
{
//  mpi_api_debug(sprockit::dbg::mpi | sprockit::dbg::mpi_collective,
//    "%s(%d,%s,%d,%s)", name,
//    count, typeStr(datatype).c_str(),
//    root, commStr(comm).c_str());

  auto op = CollectiveOp::create(count, getComm(comm));
  void* sendbuf, *recvbuf;
  op->root = root;
  MPI_Datatype sendtype, recvtype;
  if (op->comm->rank() == root){
    sendbuf = buffer;
    recvbuf = nullptr;
    sendtype = datatype;
    recvtype = MPI_DATATYPE_NULL;
  } else {
    sendbuf = nullptr;
    recvbuf = buffer;
    sendtype = MPI_DATATYPE_NULL;
    recvtype = datatype;
  }

  startMpiCollective(Iris::sumi::Collective::bcast, sendbuf, recvbuf, sendtype, recvtype, op.get());
  auto* msg = startBcast(op.get());
  if (msg){
    op->complete = true;
    delete msg;
  }
  return std::move(op);
}

int
MpiApi::bcast(void* buffer, int count, MPI_Datatype type, int root, MPI_Comm comm)
{
#ifdef SST_HG_OTF2_ENABLED
  auto start_clock = traceClock();
#endif
  do_coll(Bcast, MPI_Bcast, comm,
           count, type, root, buffer);

#ifdef SST_HG_OTF2_ENABLED
  if(OTF2Writer_) {
    MpiComm* commPtr = getComm(comm);
    OTF2Writer_->writer().mpi_bcast(start_clock, traceClock(),
        count, type, root, comm);
  }
#endif

  return MPI_SUCCESS;
}

int
MpiApi::bcast(int count, MPI_Datatype datatype, int root, MPI_Comm comm)
{
  return bcast(NULL, count, datatype, root, comm);
}

int
MpiApi::ibcast(void* buffer, int count, MPI_Datatype type, int root,
                MPI_Comm comm, MPI_Request* req)
{

  start_coll(Bcast, MPI_Ibcast, comm, count, type, root, buffer);
  return MPI_SUCCESS;
}

int
MpiApi::ibcast(int count, MPI_Datatype datatype, int root,
                MPI_Comm comm, MPI_Request* req)
{
  return ibcast(NULL, count, datatype, root, comm, req);
}

Iris::sumi::CollectiveDoneMessage*
MpiApi::startGather(CollectiveOp* op)
{
  return engine_->gather(op->root, op->tmp_recvbuf, op->tmp_sendbuf, op->sendcnt,
                    op->sendtype->packed_size(), op->tag,
                    queue_->collCqId(), op->comm);
}

CollectiveOpBase::ptr
MpiApi::startGather(const char* name, MPI_Comm comm, int sendcount, MPI_Datatype sendtype, int root,
                      int recvcount, MPI_Datatype recvtype, const void *sendbuf, void *recvbuf)
{
//  mpi_api_debug(sprockit::dbg::mpi | sprockit::dbg::mpi_collective,
//    "%s(%d,%s,%d,%s,%s)", name,
//    sendcount, typeStr(sendtype).c_str(),
//    recvcount, typeStr(recvtype).c_str(),
//    commStr(comm).c_str());

  if (sendbuf == MPI_IN_PLACE){
    if (recvbuf){
      MpiType* type = typeFromId(recvtype);
      MpiComm* cm = getComm(comm);
      int offset = type->extent() * recvcount * cm->rank();
      sendbuf = ((char*)recvbuf) + offset;
    }
    sendcount = recvcount;
    sendtype = recvtype;
  }

  auto op = CollectiveOp::create(sendcount, recvcount, getComm(comm));
  op->root = root;

  if (root == op->comm->rank()){
    //pass
  } else {
    recvtype = MPI_DATATYPE_NULL;
    recvbuf = nullptr;
  }

  startMpiCollective(Iris::sumi::Collective::gather, sendbuf, recvbuf, sendtype, recvtype, op.get());
  auto* msg = startGather(op.get());
  if (msg){
    op->complete = true;
    delete msg;
  }
  return std::move(op);
}

int
MpiApi::gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm)
{
#ifdef SST_HG_OTF2_ENABLED
  auto start_clock = traceClock();
#endif
  do_coll(Gather, MPI_Gather, comm, sendcount, sendtype, root,
          recvcount, recvtype, sendbuf, recvbuf);

#ifdef SST_HG_OTF2_ENABLED
  if(OTF2Writer_){
    OTF2Writer_->writer().mpi_gather(start_clock, traceClock(),
        sendcount, sendtype, recvcount, recvtype, root, comm);
  }
#endif

  return MPI_SUCCESS;
}

int
MpiApi::gather(int sendcount, MPI_Datatype sendtype,
                int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm)
{
  return gather(NULL, sendcount, sendtype, NULL, recvcount, recvtype, root, comm);
}

int
MpiApi::igather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, int recvcount, MPI_Datatype recvtype, int root,
                MPI_Comm comm, MPI_Request* req)
{
  start_coll(Gather, MPI_Igather, comm, sendcount, sendtype, root,
             recvcount, recvtype, sendbuf, recvbuf);
  return MPI_SUCCESS;
}

int
MpiApi::igather(int sendcount, MPI_Datatype sendtype,
                int recvcount, MPI_Datatype recvtype, int root,
                MPI_Comm comm, MPI_Request* req)
{
  return igather(NULL, sendcount, sendtype, NULL,
                 recvcount, recvtype, root, comm, req);
}

Iris::sumi::reduce_fxn
MpiApi::getCollectiveFunction(CollectiveOpBase* op)
{
  if (op->op >= first_custom_op_id){
    auto iter = custom_ops_.find(op->op);
    if (iter == custom_ops_.end()){
      sst_hg_throw_printf(SST::Hg::ValueError,
                        "Got invalid MPI_Op %d",
                        op->op);
   }
    MPI_User_function* mpifxn = iter->second;
    MPI_Datatype dtype = op->sendtype->id;
    Iris::sumi::reduce_fxn fxn = ([=](void* dst, const void* src, int count){
      MPI_Datatype copy_type = dtype;
      (*mpifxn)(const_cast<void*>(src), dst, &count, &copy_type);
    });
    return fxn;
  } else if (op->tmp_sendbuf){
    return op->sendtype->op(op->op);
  } else {
    //the function is irrelevant
    //just give it the integer add - function
    return &ReduceOp<Add,int>::op;
  }
}

Iris::sumi::CollectiveDoneMessage*
MpiApi::startReduce(CollectiveOp* op)
{
  Iris::sumi::reduce_fxn fxn = getCollectiveFunction(op);
  return engine_->reduce(op->root, op->tmp_recvbuf, op->tmp_sendbuf, op->sendcnt,
                    op->sendtype->packed_size(), op->tag,
                    fxn, queue_->collCqId(), op->comm);
}

CollectiveOpBase::ptr
MpiApi::startReduce(const char* name, MPI_Comm comm, int count, MPI_Datatype type, int root,
                      MPI_Op mop, const void* src, void* dst)
{
//  mpi_api_debug(sprockit::dbg::mpi | sprockit::dbg::mpi_collective,
//    "%s(%d,%s,%d,%s)", name,
//    count, typeStr(type).c_str(),
//    root,  commStr(comm).c_str());

  auto op = CollectiveOp::create(count, getComm(comm));
  op->root = root;
  op->op = mop;
  MPI_Datatype sendtype, recvtype;
  if (root == op->comm->rank()){
    sendtype = type;
    recvtype = type;
  } else {
    sendtype = type;
    recvtype = MPI_DATATYPE_NULL;
    dst = nullptr;
  }

  startMpiCollective(Iris::sumi::Collective::reduce, src, dst, sendtype, recvtype, op.get());
  auto* msg = startReduce(op.get());
  if (msg){
    op->complete = true;
    delete msg;
  }
  return std::move(op);
}

int
MpiApi::reduce(const void *src, void *dst, int count,
                MPI_Datatype type, MPI_Op mop, int root, MPI_Comm comm)
{
#ifdef SST_HG_OTF2_ENABLED
  auto start_clock = traceClock();
#endif
  do_coll(Reduce, MPI_Reduce, comm, count,
          type, root, mop, src, dst);

#ifdef SST_HG_OTF2_ENABLED
  if(OTF2Writer_){
    OTF2Writer_->writer().mpi_reduce(start_clock, traceClock(),
      count, type, root, comm);
  }
#endif

  return MPI_SUCCESS;
}

int
MpiApi::reduce(int count, MPI_Datatype type, MPI_Op op, int root, MPI_Comm comm)
{
  return reduce(NULL, NULL, count, type, op, root, comm);
}

int
MpiApi::ireduce(const void* sendbuf, void* recvbuf, int count,
                 MPI_Datatype type, MPI_Op mop, int root, MPI_Comm comm,
                 MPI_Request* req)
{
  start_coll(Reduce, MPI_Ireduce, comm, count,
             type, root, mop, sendbuf, recvbuf);
  return MPI_SUCCESS;
}

int
MpiApi::ireduce(int count, MPI_Datatype type, MPI_Op op, int root, MPI_Comm comm, MPI_Request* req)
{
  return ireduce(NULL, NULL, count, type, op, root, comm, req);
}

Iris::sumi::CollectiveDoneMessage*
MpiApi::startReduceScatter(CollectiveOp* op)
{
  SST::Hg::abort("sumi::reduce_scatter");

  Iris::sumi::reduce_fxn fxn = getCollectiveFunction(op);
  return nullptr;
  //transport::allreduce(op->tmp_recvbuf, op->tmp_sendbuf, op->sendcnt,
  //                     op->sendtype->packed_size(), op->tag,
  //                     fxn, false, options::initial_context, op->comm);

}

CollectiveOpBase::ptr
MpiApi::startReduceScatter(const char*  /*name*/, MPI_Comm  /*comm*/, const int*  /*recvcounts*/,
                           MPI_Datatype type, MPI_Op mop, const void* src, void* dst)
{
  SST::Hg::abort("sumi::reduce_scatter");

  CollectiveOp::ptr op;
  startMpiCollective(Iris::sumi::Collective::reduce_scatter, src, dst, type, type, op.get());
  auto* msg = startReduceScatter(op.get());
  if (msg){
    op->complete = true;
    delete msg;
  }
  op->op = mop;

  return std::move(op);
}

int
MpiApi::reduceScatter(const void *src, void *dst, const int *recvcnts,
                        MPI_Datatype type, MPI_Op mop, MPI_Comm comm)
{
#ifdef SST_HG_OTF2_ENABLED
  auto start_clock = traceClock();
#endif
  do_coll(ReduceScatter, MPI_Reduce_scatter,
          comm, recvcnts, type, mop, src, dst);

#ifdef SST_HG_OTF2_ENABLED
  if (OTF2Writer_){
    OTF2Writer_->writer().mpi_reduce_scatter(start_clock, traceClock(),
          getComm(comm)->size(), recvcnts, type, comm);
  }
#endif

  return MPI_SUCCESS;
}

int
MpiApi::reduceScatter(int *recvcnts, MPI_Datatype type, MPI_Op op, MPI_Comm comm)
{
  return reduceScatter(NULL, NULL, recvcnts, type, op, comm);
}

int
MpiApi::ireduceScatter(const void *src, void *dst, const int *recvcnts,
                        MPI_Datatype type, MPI_Op mop,
                        MPI_Comm comm, MPI_Request* req)
{
  start_coll(ReduceScatter, MPI_Ireduce_scatter,
             comm, recvcnts, type, mop, src, dst);
  return MPI_SUCCESS;
}

int
MpiApi::ireduceScatter(int *recvcnts, MPI_Datatype type,
                         MPI_Op op, MPI_Comm comm, MPI_Request* req)
{
  return ireduceScatter(NULL, NULL, recvcnts, type, op, comm, req);
}

CollectiveOpBase::ptr
MpiApi::startReduceScatterBlock(const char*  /*name*/, MPI_Comm  /*comm*/, int  /*count*/, MPI_Datatype type,
                                    MPI_Op mop, const void* src, void* dst)
{
  SST::Hg::abort("sumi::reduce_scatter: not implemented");

  CollectiveOp::ptr op;
  startMpiCollective(Iris::sumi::Collective::reduce_scatter, src, dst, type, type, op.get());
  auto* msg = startReduceScatter(op.get());
  op->op = mop;
  if (msg){
    op->complete = true;
    delete msg;
  }
  return std::move(op);
}

int
MpiApi::reduceScatterBlock(const void *src, void *dst, int recvcnt,
                        MPI_Datatype type, MPI_Op mop, MPI_Comm comm)
{
  do_coll(ReduceScatterBlock, MPI_Reduce_scatter_block,
        comm, recvcnt, type, mop, src, dst);
  return MPI_SUCCESS;
}

int
MpiApi::reduceScatterBlock(int recvcnt, MPI_Datatype type, MPI_Op op, MPI_Comm comm)
{
  return reduceScatterBlock(NULL, NULL, recvcnt, type, op, comm);
}

int
MpiApi::ireduceScatterBlock(const void *src, void *dst, int recvcnt,
                        MPI_Datatype type, MPI_Op mop,
                        MPI_Comm comm, MPI_Request* req)
{
  start_coll(ReduceScatterBlock,
          MPI_Ireduce_scatter_block,
          comm, recvcnt, type, mop, src, dst);
  return MPI_SUCCESS;
}

int
MpiApi::ireduceScatterBlock(int recvcnt, MPI_Datatype type,
                         MPI_Op op, MPI_Comm comm, MPI_Request* req)
{
  return ireduceScatterBlock(NULL, NULL, recvcnt, type, op, comm, req);
}

Iris::sumi::CollectiveDoneMessage*
MpiApi::startScan(CollectiveOp* op)
{
  Iris::sumi::reduce_fxn fxn = getCollectiveFunction(op);
  return engine_->scan(op->tmp_recvbuf, op->tmp_sendbuf, op->sendcnt,
                  op->sendtype->packed_size(), op->tag,
                  fxn, queue_->collCqId(), op->comm);
}

CollectiveOpBase::ptr
MpiApi::startScan(const char* name, MPI_Comm comm, int count, MPI_Datatype type,
                  MPI_Op mop, const void* src, void* dst)
{
//  mpi_api_debug(sprockit::dbg::mpi | sprockit::dbg::mpi_collective,
//    "%s(%d,%s,%s)", name,
//    count, typeStr(type).c_str(),
//    commStr(comm).c_str());

  auto op = CollectiveOp::create(count, getComm(comm));
  if (src == MPI_IN_PLACE){
    src = dst;
  }

  op->op = mop;
  startMpiCollective(Iris::sumi::Collective::scan, src, dst, type, type, op.get());
  auto* msg = startScan(op.get());
  if (msg){
    op->complete = true;
    delete msg;
  }
  //some compiler require this, despit RVO
  return std::move(op);
}

int
MpiApi::scan(const void *src, void *dst, int count, MPI_Datatype type, MPI_Op mop, MPI_Comm comm)
{
#ifdef SST_HG_OTF2_ENABLED
  auto start_clock = traceClock();
#endif
  do_coll(Scan, MPI_Scan, comm, count, type, mop, src, dst);

#ifdef SST_HG_OTF2_ENABLED
  if(OTF2Writer_){
    OTF2Writer_->writer().mpi_scan(start_clock, traceClock(), count, type, comm);
  }
#endif

  return MPI_SUCCESS;
}

int
MpiApi::scan(int count, MPI_Datatype type, MPI_Op op, MPI_Comm comm)
{
  return scan(NULL, NULL, count, type, op, comm);
}

int
MpiApi::iscan(const void *src, void *dst, int count, MPI_Datatype type,
               MPI_Op mop, MPI_Comm comm, MPI_Request* req)
{
  start_coll(Scan, MPI_Iscan, comm, count, type, mop, src, dst);
  return MPI_SUCCESS;
}

int
MpiApi::iscan(int count, MPI_Datatype type, MPI_Op op,
               MPI_Comm comm, MPI_Request* req)
{
  return iscan(NULL, NULL, count, type, op, comm, req);
}

Iris::sumi::CollectiveDoneMessage*
MpiApi::startScatter(CollectiveOp* op)
{
  return engine_->scatter(op->root, op->tmp_recvbuf, op->tmp_sendbuf, op->sendcnt,
                     op->sendtype->packed_size(), op->tag,
                     queue_->collCqId(), op->comm);
}

CollectiveOpBase::ptr
MpiApi::startScatter(const char* name, MPI_Comm comm, int sendcount, MPI_Datatype sendtype, int root,
                     int recvcount, MPI_Datatype recvtype, const void *sendbuf, void *recvbuf)
{
//  mpi_api_debug(sprockit::dbg::mpi | sprockit::dbg::mpi_collective,
//    "%s(%d,%s,%d,%s)", name,
//    sendcount, typeStr(sendtype).c_str(),
//    recvcount, typeStr(recvtype).c_str(),
//    commStr(comm).c_str());

  auto op = CollectiveOp::create(sendcount, recvcount, getComm(comm));

  op->root = root;
  if (root == op->comm->rank()){
    //pass
  } else {
    sendtype = MPI_DATATYPE_NULL;
    sendbuf = nullptr;
  }

  startMpiCollective(Iris::sumi::Collective::scatter, sendbuf, recvbuf, sendtype, recvtype, op.get());
  auto* msg = startScatter(op.get());
  if (msg){
    op->complete = true;
    delete msg;
  }
  //move required, instead of RVO, for some compilers
  return std::move(op);
}

int
MpiApi::scatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype, int root,
                 MPI_Comm comm)
{
#ifdef SST_HG_OTF2_ENABLED
  auto start_clock = traceClock();
#endif
  do_coll(Scatter, MPI_Scatter, comm, sendcount, sendtype, root,
          recvcount, recvtype, sendbuf, recvbuf);

#ifdef SST_HG_OTF2_ENABLED
  if (OTF2Writer_){
    OTF2Writer_->writer().mpi_scatter(start_clock, traceClock(),
      sendcount, sendtype, recvcount, recvtype, root, comm);
  }
#endif

  return MPI_SUCCESS;
}

int
MpiApi::scatter(int sendcount, MPI_Datatype sendtype,
                 int recvcount, MPI_Datatype recvtype, int root, MPI_Comm comm)
{
  return scatter(NULL, sendcount, sendtype, NULL, recvcount, recvtype, root, comm);
}

int
MpiApi::iscatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype, int root,
                 MPI_Comm comm, MPI_Request* req)
{
  start_coll(Scatter, MPI_Iscatter, comm, sendcount, sendtype, root,
             recvcount, recvtype, sendbuf, recvbuf);
  return MPI_SUCCESS;
}

int
MpiApi::iscatter(int sendcount, MPI_Datatype sendtype,
                 int recvcount, MPI_Datatype recvtype,
                 int root, MPI_Comm comm, MPI_Request* req)
{
  return iscatter(NULL, sendcount, sendtype,
                 NULL, recvcount, recvtype,
                 root, comm, req);
}

}
