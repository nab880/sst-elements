// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include <sst_config.h>

#include <mercury/components/collective_adapter.h>
#include <mercury/components/nic.h>
#include <mercury/components/node_base.h>

#include <limits>

namespace SST::Hg {
namespace {

using namespace SST::Collective;
using SimpleNetwork = SST::Interfaces::SimpleNetwork;

} // namespace

MercuryCollectiveAdapter::MercuryCollectiveAdapter(NIC& owner,
    SimpleNetwork& network, StaticConfig config) :
  owner_(owner),
  network_(network),
  config_(config)
{
}

bool MercuryCollectiveAdapter::transportAvailable() const
{
  if (config_.job_namespace == 0 || config_.root_physical_nid < 0 ||
      config_.root_logical_nid < 0 || config_.reduce_vn < 0 ||
      config_.result_vn < 0 || config_.reduce_vn == config_.result_vn) {
    return false;
  }

  return supportsStaticCollectiveTransport(
      network_, config_.reduce_vn, config_.result_vn);
}

bool MercuryCollectiveAdapter::installStaticRoute(
    SimpleNetwork::nid_t physical_endpoint_id,
    SimpleNetwork::nid_t participant_logical_id)
{
  if (participant() != nullptr || !transportAvailable() || physical_endpoint_id < 0 ||
      participant_logical_id < 0 || network_.getEndpointID() < 0) {
    return false;
  }

  const RouteIdV1 route {config_.job_namespace, config_.route_id};
  const auto endpoint_logical_nid = network_.getEndpointID();
  if (!route.valid() || endpoint_logical_nid < 0) return false;

  CollectiveParticipant installed;
  installed.route = route;
  installed.physical_endpoint_id = physical_endpoint_id;
  installed.logical_participant_id = participant_logical_id;
  installed.reduce_vn = config_.reduce_vn;
  installed.result_vn = config_.result_vn;
  if (!installParticipant(installed)) return false;

  endpoint_logical_nid_ = endpoint_logical_nid;
  return true;
}

bool MercuryCollectiveAdapter::transportReady(
    const CollectiveSignatureV1& signature) const
{
  const auto request_bits = staticCollectiveRequestBits(signature);
  if (!request_bits || *request_bits > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  return network_.spaceToSend(
      config_.reduce_vn, static_cast<int>(*request_bits));
}

void MercuryCollectiveAdapter::commitContribution(
    StaticCollectiveContribution&& contribution) noexcept
{
  // Mercury's LinkControl translates logical destinations, so the root is
  // addressed by its logical NID.
  staging_bytes_ = contribution.value.size();
  pending_request_ = makeStaticCollectiveContributionRequest(contribution,
      config_.root_logical_nid, endpoint_logical_nid_, config_.reduce_vn);
  if (!pending_request_) {
    sst_hg_abort_printf("Mercury constructed an invalid collective contribution\n");
  }
  // Accepted already owns a source snapshot. Keep the request while the
  // shared host-memory model stages it, then wait for network credits if needed.
  staging_contribution_ = true;
  owner_.sendDelayedExecutionEvent(TimeDelta(config_.submit_delay_ns, TimeDelta::one_nanosecond),
      newCallback(this, &MercuryCollectiveAdapter::stageContribution));
}

void MercuryCollectiveAdapter::stageContribution()
{
  owner_.parent_->accessHostMemory(staging_bytes_,
      newCallback(this, &MercuryCollectiveAdapter::contributionStaged));
}

void MercuryCollectiveAdapter::contributionStaged()
{
  staging_contribution_ = false;
  staging_bytes_ = 0;
  sendContribution();
}

void MercuryCollectiveAdapter::sendContribution()
{
  if (pending_request_ && !staging_contribution_ &&
      owner_.trySendCollective(pending_request_.get(), config_.reduce_vn)) {
    pending_request_.release();
  }
}

void MercuryCollectiveAdapter::sendNotification(int vn)
{
  if (vn == config_.reduce_vn) {
    sendContribution();
    notifyReadyIfPossible();
  }
}

bool MercuryCollectiveAdapter::receiveResult(
    int vn, const SimpleNetwork::Request& request)
{
  if (activeInvocation() == 0 || vn != config_.result_vn || pending_result_ || pending_request_) return false;
  auto result = inspectStaticCollectiveResult(request,
      installedParticipant().route, activeInvocation(), config_.root_logical_nid,
      endpoint_logical_nid_, config_.result_vn);
  if (!result) return false;
  pending_result_ = std::move(*result);
  owner_.parent_->accessHostMemory(pending_result_->value.size(),
      newCallback(this, &MercuryCollectiveAdapter::resultStaged));
  return true;
}

void MercuryCollectiveAdapter::resultStaged()
{
  owner_.sendDelayedExecutionEvent(TimeDelta(config_.completion_delay_ns, TimeDelta::one_nanosecond),
      newCallback(this, &MercuryCollectiveAdapter::finishResult));
}

void MercuryCollectiveAdapter::finishResult()
{
  // Release pending state before the sink can submit its next invocation.
  auto result = std::move(*pending_result_);
  pending_result_.reset();
  if (!completeSuccess(result)) {
    sst_hg_abort_printf("Mercury collective memory completion lost its invocation\n");
  }
}

} // namespace SST::Hg
