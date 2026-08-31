// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include <sst_config.h>

#include <mercury/components/collective_adapter.h>
#include <mercury/components/nic.h>

#include <sst/elements/merlin/services/collective/collectiveServiceData.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace SST::Hg {
namespace {

using namespace SST::Collective;
using SimpleNetwork = SST::Interfaces::SimpleNetwork;

bool sameParticipant(
    const AcceptedParticipantHandle& lhs,
    const AcceptedParticipantHandle& rhs)
{
  const bool same_fabric =
      lhs.fabric.has_value() == rhs.fabric.has_value() &&
      (!lhs.fabric ||
          (lhs.fabric->endpoint_reduce_vn == rhs.fabric->endpoint_reduce_vn &&
           lhs.fabric->endpoint_result_vn == rhs.fabric->endpoint_result_vn &&
           lhs.fabric->injection_dest_nid == rhs.fabric->injection_dest_nid));
  return lhs.schema_version == rhs.schema_version && lhs.route == rhs.route &&
         lhs.physical_route == rhs.physical_route &&
         lhs.route_kind == rhs.route_kind && lhs.data_mode == rhs.data_mode &&
         lhs.physical_endpoint_id == rhs.physical_endpoint_id &&
         lhs.local_participant_slot == rhs.local_participant_slot &&
         lhs.local_participant_count == rhs.local_participant_count &&
         lhs.logical_participant_id == rhs.logical_participant_id &&
         lhs.binding == rhs.binding &&
         lhs.accepted_invocation_quota == rhs.accepted_invocation_quota &&
         lhs.submission_window == rhs.submission_window && same_fabric;
}

} // namespace

MercuryCollectiveAdapter::MercuryCollectiveAdapter(NIC& owner,
    SimpleNetwork& network, uint64_t owner_component_id, StaticConfig config) :
  owner_(owner),
  network_(network),
  owner_component_id_(owner_component_id),
  config_(config)
{
}

bool MercuryCollectiveAdapter::transportAvailable() const
{
  if (config_.job_namespace == 0 || config_.root_physical_nid < 0 ||
      config_.root_logical_nid < 0 ||
      config_.reduce_vn != 0 || config_.result_vn != 1) {
    return false;
  }

  const auto request_bits = staticCollectiveRequestBits(STATIC_COLLECTIVE_SIGNATURE_V1);
  if (!request_bits ||
      *request_bits > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
      *request_bits > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }

  SimpleNetwork::NetworkServiceCapability capability;
  constexpr SimpleNetwork::NetworkServiceFeatureMask required =
      SimpleNetwork::SERVICE_FEATURE_SIDECAR_PRESERVATION |
      SimpleNetwork::SERVICE_FEATURE_TRANSACTIONAL_TIMED_SEND |
      SimpleNetwork::SERVICE_FEATURE_SERIALIZATION |
      SimpleNetwork::SERVICE_FEATURE_INTERMEDIATE_TERMINATION_SAFE |
      SimpleNetwork::SERVICE_FEATURE_FRESH_BASE_REQUEST_TAG_FIRST_RECEIVE;
  const size_t maximum_vn = static_cast<size_t>(
      std::max(config_.reduce_vn, config_.result_vn));
  return network_.queryServiceCapability(COLLECTIVE_SERVICE_ID, capability) &&
         capability.isValidFor(COLLECTIVE_SERVICE_ID) &&
         (capability.features & required) == required &&
         capability.min_schema_version <= COLLECTIVE_SERVICE_SCHEMA_V1 &&
         capability.max_schema_version >= COLLECTIVE_SERVICE_SCHEMA_V1 &&
         capability.request_data_token == CollectiveServiceData::DATA_TOKEN &&
         capability.min_request_schema_version <= CollectiveServiceData::MIN_SCHEMA_VERSION &&
         capability.max_request_schema_version >= CollectiveServiceData::MAX_SCHEMA_VERSION &&
         capability.max_atomic_request_bits_by_vn.size() > maximum_vn &&
         capability.max_atomic_request_bits_by_vn[config_.reduce_vn] >= *request_bits &&
         capability.max_atomic_request_bits_by_vn[config_.result_vn] >= *request_bits;
}

bool MercuryCollectiveAdapter::installStaticRoute(
    SimpleNetwork::nid_t physical_endpoint_id,
    SimpleNetwork::nid_t participant_logical_id)
{
  if (installed_ || !transportAvailable() || physical_endpoint_id < 0 ||
      participant_logical_id < 0 || network_.getEndpointID() < 0) {
    return false;
  }

  const RouteIdV1 route {config_.job_namespace, config_.route_id};
  const auto endpoint_logical_nid = network_.getEndpointID();
  if (!route.valid() || endpoint_logical_nid < 0) return false;

  route_ = route;
  accepted_.route = route_;
  accepted_.physical_route = {0, 1};
  accepted_.route_kind = CollectiveRouteKind::FabricTree;
  accepted_.data_mode = CollectiveDataMode::Functional;
  accepted_.physical_endpoint_id = physical_endpoint_id;
  accepted_.local_participant_slot = 0;
  accepted_.local_participant_count = 1;
  accepted_.logical_participant_id = static_cast<uint64_t>(participant_logical_id);
  accepted_.binding = {owner_component_id_, 0, 1};
  accepted_.accepted_invocation_quota = 1;
  accepted_.submission_window = 1;
  accepted_.fabric.emplace();
  accepted_.fabric->endpoint_reduce_vn = config_.reduce_vn;
  accepted_.fabric->endpoint_result_vn = config_.result_vn;
  accepted_.fabric->injection_dest_nid = config_.root_logical_nid;
  if (!accepted_.valid()) return false;

  endpoint_logical_nid_ = endpoint_logical_nid;
  installed_ = true;
  return true;
}

const AcceptedParticipantHandle* MercuryCollectiveAdapter::participant(
    uint32_t local_slot) const
{
  return installed_ && local_slot == 0 ? &accepted_ : nullptr;
}

bool MercuryCollectiveAdapter::supportsCollective(
    const CollectiveSignatureV1& signature) const
{
  return signature.valid() && signature == STATIC_COLLECTIVE_SIGNATURE_V1;
}

bool MercuryCollectiveAdapter::bindParticipant(
    const AcceptedParticipantHandle& participant,
    CollectiveCompletionSink& completion, CollectiveReadySink& ready)
{
  if (!installed_ || completion_ != nullptr || &participant != &accepted_ ||
      !participant.valid() || !sameParticipant(participant, accepted_) ||
      endpoint_logical_nid_ < 0 || !accepted_.fabric ||
      accepted_.fabric->endpoint_reduce_vn != static_cast<uint32_t>(config_.reduce_vn) ||
      accepted_.fabric->endpoint_result_vn != static_cast<uint32_t>(config_.result_vn) ||
      accepted_.fabric->injection_dest_nid != config_.root_logical_nid) {
    return false;
  }
  completion_ = &completion;
  ready_ = &ready;
  return true;
}

CollectiveSubmitResult MercuryCollectiveAdapter::trySubmitCollective(
    CollectivePending& pending)
{
  if (!installed_ || completion_ == nullptr || !pending.readyForSubmit() ||
      !pending.participant.valid() ||
      !sameParticipant(pending.participant, accepted_)) {
    return CollectiveSubmitResult::Invalid;
  }
  if (!pending.signature.valid()) return CollectiveSubmitResult::Invalid;
  if (!supportsCollective(pending.signature)) return CollectiveSubmitResult::Unsupported;
  const auto payload_bytes = pending.signature.payloadBytes();
  if (pending.invocation_id == 0 || pending.invocation_id <= completed_invocation_ ||
      !payload_bytes || pending.source.data == nullptr ||
      pending.source.bytes != *payload_bytes || pending.result.data == nullptr ||
      pending.result.bytes != *payload_bytes) {
    return CollectiveSubmitResult::Invalid;
  }
  if (active_) return CollectiveSubmitResult::Retry;

  StaticCollectiveContribution contribution;
  contribution.route = route_;
  contribution.invocation_id = pending.invocation_id;
  contribution.signature = pending.signature;
  contribution.value.resize(*payload_bytes);
  std::memcpy(contribution.value.data(), pending.source.data, contribution.value.size());
  auto request = makeStaticCollectiveContributionRequest(contribution,
      accepted_.fabric->injection_dest_nid, endpoint_logical_nid_, config_.reduce_vn);
  if (!request) return CollectiveSubmitResult::Invalid;
  if (!owner_.trySendCollective(request.get(), config_.reduce_vn)) {
    return CollectiveSubmitResult::Retry;
  }
  request.release();

  result_ = pending.result;
  active_signature_ = pending.signature;
  active_invocation_ = pending.invocation_id;
  token_.emplace(pending.consumeAfterAcceptance());
  active_ = true;
  ready_armed_ = false;
  return CollectiveSubmitResult::Accepted;
}

void MercuryCollectiveAdapter::requestCollectiveReady(
    const AcceptedParticipantHandle& participant,
    const CollectiveSignatureV1& signature)
{
  if (!installed_ || ready_ == nullptr || !participant.valid() ||
      !sameParticipant(participant, accepted_) || !supportsCollective(signature)) {
    return;
  }
  ready_armed_ = true;
  ready_signature_ = signature;
  notifyReadyIfPossible();
}

void MercuryCollectiveAdapter::notifyReadyIfPossible()
{
  const auto request_bits = staticCollectiveRequestBits(ready_signature_);
  if (!ready_armed_ || active_ || ready_ == nullptr ||
      !request_bits ||
      *request_bits > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
      !network_.spaceToSend(config_.reduce_vn, static_cast<int>(*request_bits))) {
    return;
  }
  ready_armed_ = false;
  ready_signature_ = {};
  ready_->ready(accepted_);
}

void MercuryCollectiveAdapter::sendNotification(int vn)
{
  if (vn == config_.reduce_vn) notifyReadyIfPossible();
}

bool MercuryCollectiveAdapter::receiveResult(
    int vn, const SimpleNetwork::Request& request)
{
  if (!active_ || !token_ || completion_ == nullptr ||
      vn != config_.result_vn) {
    return false;
  }
  const auto completed = inspectStaticCollectiveResult(request, route_,
      active_invocation_, config_.root_logical_nid, endpoint_logical_nid_, config_.result_vn);
  if (!completed || completed->signature != active_signature_ ||
      result_.data == nullptr || result_.bytes != completed->value.size()) {
    return false;
  }

  std::memcpy(result_.data, completed->value.data(), completed->value.size());
  CollectiveCompletionToken token(std::move(*token_));
  token_.reset();
  result_ = {};
  active_signature_ = {};
  completed_invocation_ = active_invocation_;
  active_invocation_ = 0;
  active_ = false;
  completion_->complete(std::move(token), CollectiveCompletionStatus::Success);
  notifyReadyIfPossible();
  return true;
}

bool MercuryCollectiveAdapter::quiescent() const
{
  return !active_ && !active_signature_.valid() && !token_.has_value();
}

} // namespace SST::Hg
