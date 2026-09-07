/**
Copyright 2009-2026 National Technology and Engineering Solutions of Sandia,
LLC (NTESS).  Under the terms of Contract DE-NA-0003525 with NTESS, the U.S.
Government retains certain rights in this software.

Copyright (c) 2009-2026, NTESS
All rights reserved.
*/

#include <mpi_collective_offload.h>

#include <mpi_api.h>
#include <mercury/components/operating_system.h>
#include <mercury/operating_system/process/app.h>
#include <mercury/operating_system/process/thread.h>

#include <limits>
#include <optional>

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

MpiCollectiveOffload::MpiCollectiveOffload(MpiApi& api, bool enabled) :
  api_(api),
  enabled_(enabled)
{
}

bool
MpiCollectiveOffload::bind()
{
  if (endpoint_) return true;

  auto* operating_system = api_.parent()->os();
  auto* endpoint = operating_system ? operating_system->collectiveEndpoint() : nullptr;
  const auto* participant = endpoint ? endpoint->participant() : nullptr;
  if (!endpoint || !participant || !participant->valid() || !api_.worldcomm_ ||
      participant->logical_participant_id != static_cast<int64_t>(api_.worldcomm_->rank())) {
    return false;
  }

  if (!endpoint->bind(*this, *this)) return false;
  endpoint_ = endpoint;
  return true;
}

void
MpiCollectiveOffload::clearRequest()
{
  if (request_comm_ && request_ &&
      !request_comm_->removeRequest(request_tag_, request_)) {
    sst_hg_abort_printf("Mask-MPI collective offload lost its registered native request");
  }
  request_ = nullptr;
  request_comm_ = nullptr;
  waiter_ = nullptr;
  waiting_blocked_ = false;
  request_tag_ = 0;
  request_invocation_ = 0;
  ready_ = false;
}

void
MpiCollectiveOffload::complete(uint64_t invocation_id,
                               SST::Collective::CollectiveCompletionStatus status)
{
  if (!SST::Collective::isValid(status) || !request_ || !request_comm_ ||
      invocation_id == 0 || invocation_id != request_invocation_ ||
      request_comm_->getRequest(request_tag_) != request_ ||
      request_->isComplete()) {
    sst_hg_abort_printf("Mask-MPI received an invalid or duplicate collective offload completion");
  }

  request_->complete();
  if (waiter_ && waiting_blocked_) {
    api_.parent()->os()->unblock(waiter_);
  }
}

void
MpiCollectiveOffload::ready()
{
  if (!request_) {
    sst_hg_abort_printf("Mask-MPI received a collective offload ready notification without a request");
  }
  ready_ = true;
  if (waiter_ && waiting_blocked_) {
    api_.parent()->os()->unblock(waiter_);
  }
}

bool
MpiCollectiveOffload::tryBlockingAllreduce(CollectiveOp::ptr& op, MPI_Datatype type, MPI_Op mop)
{
  if (!enabled_) {
    return false;
  }
  auto* operating_system = api_.parent()->os();
  if (op->comm != api_.worldcomm_ || op->comm->id() != MPI_COMM_WORLD ||
      op->sendcnt != op->recvcnt) {
    return false;
  }
  const auto mapped_signature = mapCollectiveSignature(
      mop, type, op->sendcnt, op->sendtype->packed_size());
  if (!mapped_signature) return false;

  auto* candidate_endpoint = endpoint_ != nullptr ? endpoint_ :
      (operating_system != nullptr ? operating_system->collectiveEndpoint() : nullptr);
  if (candidate_endpoint != nullptr &&
      !candidate_endpoint->supportsCollective(mapped_signature->signature)) {
    return false;
  }

  // Per-call properties the static profile cannot carry (noncontiguous
  // datatypes, timing-only buffers) use the software collective.
  if (op->packed_send || op->packed_recv || op->tmp_sendbuf == nullptr ||
      op->tmp_recvbuf == nullptr) {
    return false;
  }
  // A configuration that cannot host the endpoint at all is an error: the
  // user asked for offload and would otherwise silently measure software.
  if (!operating_system || operating_system->ranksPerNode() != 1) {
    sst_hg_abort_printf(
        "Mask-MPI collective offload is enabled but this node cannot host the static endpoint");
  }
  if (!bind()) {
    sst_hg_abort_printf("Mask-MPI collective offload is enabled but no valid endpoint is available");
  }
  if (request_ != nullptr || op->tag < 0) {
    sst_hg_abort_printf("Mask-MPI collective offload supports one outstanding blocking call");
  }

  const uint64_t invocation_id = next_invocation_++;
  if (invocation_id == 0 || next_invocation_ == 0) {
    sst_hg_abort_printf("Mask-MPI collective offload invocation sequence exhausted");
  }

  auto* request = MpiRequest::construct(MpiRequest::Collective);
  request_ = request;
  request_comm_ = op->comm;
  request_tag_ = op->tag;
  request_invocation_ = invocation_id;
  op->comm->addRequest(op->tag, request);

  SST::Collective::CollectiveSubmission submission;
  submission.invocation_id = invocation_id;
  submission.signature = mapped_signature->signature;
  submission.source = {reinterpret_cast<const uint8_t*>(op->tmp_sendbuf),
                       mapped_signature->payload_bytes};
  submission.result = {reinterpret_cast<uint8_t*>(op->tmp_recvbuf),
                       mapped_signature->payload_bytes};

  while (true) {
    const auto result = endpoint_->trySubmitCollective(submission);
    if (result == SST::Collective::CollectiveSubmitResult::Accepted) {
      request->setCollective(std::move(op));
      waiter_ = operating_system->activeThread();
      if (!waiter_) {
        sst_hg_abort_printf("Mask-MPI collective offload has no active application thread");
      }
      while (!request->isComplete()) {
        waiting_blocked_ = true;
        operating_system->block();
        waiting_blocked_ = false;
      }
      waiter_ = nullptr;

      api_.finishCollective(request->collectiveData());
      clearRequest();
      delete request;
      return true;
    }

    if (result == SST::Collective::CollectiveSubmitResult::Unsupported) {
      clearRequest();
      delete request;
      return false;
    }
    if (result == SST::Collective::CollectiveSubmitResult::Invalid) {
      clearRequest();
      delete request;
      sst_hg_abort_printf("Mask-MPI collective endpoint rejected a valid submission");
    }
    if (result != SST::Collective::CollectiveSubmitResult::Retry) {
      sst_hg_abort_printf("Mask-MPI collective endpoint returned an unknown status");
    }

    ready_ = false;
    waiter_ = operating_system->activeThread();
    if (!waiter_) {
      sst_hg_abort_printf("Mask-MPI collective retry has no active application thread");
    }
    endpoint_->requestCollectiveReady(submission.signature);
    while (!ready_) {
      waiting_blocked_ = true;
      operating_system->block();
      waiting_blocked_ = false;
    }
    waiter_ = nullptr;
  }
}


} // namespace SST::MASKMPI
