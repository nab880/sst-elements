// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include <sst_config.h>

#include <mercury/components/collective_adapter.h>
#include <mercury/components/nic.h>

#include <limits>

namespace SST::Hg {
namespace {

using namespace SST::Collective;
using SimpleNetwork = SST::Interfaces::SimpleNetwork;

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

  return supportsStaticCollectiveTransport(
      network_, config_.reduce_vn, config_.result_vn);
}

bool MercuryCollectiveAdapter::installStaticRoute(
    SimpleNetwork::nid_t physical_endpoint_id,
    SimpleNetwork::nid_t participant_logical_id)
{
  if (participant(0) != nullptr || !transportAvailable() || physical_endpoint_id < 0 ||
      participant_logical_id < 0 || network_.getEndpointID() < 0) {
    return false;
  }

  const RouteIdV1 route {config_.job_namespace, config_.route_id};
  const auto endpoint_logical_nid = network_.getEndpointID();
  if (!route.valid() || endpoint_logical_nid < 0) return false;

  AcceptedParticipantHandle accepted;
  accepted.route = route;
  accepted.physical_route = {0, 1};
  accepted.route_kind = CollectiveRouteKind::FabricTree;
  accepted.data_mode = CollectiveDataMode::Functional;
  accepted.physical_endpoint_id = physical_endpoint_id;
  accepted.local_participant_slot = 0;
  accepted.local_participant_count = 1;
  accepted.logical_participant_id = static_cast<uint64_t>(participant_logical_id);
  accepted.binding = {owner_component_id_, 0, 1};
  accepted.accepted_invocation_quota = 1;
  accepted.submission_window = 1;
  accepted.fabric.emplace();
  accepted.fabric->endpoint_reduce_vn = config_.reduce_vn;
  accepted.fabric->endpoint_result_vn = config_.result_vn;
  accepted.fabric->injection_dest_nid = config_.root_logical_nid;
  if (!installParticipant(accepted)) return false;

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
    const AcceptedParticipantHandle& participant,
    StaticCollectiveContribution&& contribution) noexcept
{
  auto request = makeStaticCollectiveContributionRequest(contribution,
      participant.fabric->injection_dest_nid, endpoint_logical_nid_,
      config_.reduce_vn);
  if (!request) {
    sst_hg_abort_printf("Mercury constructed an invalid collective contribution\n");
  }
  if (!owner_.trySendCollective(request.get(), config_.reduce_vn)) {
    sst_hg_abort_printf(
        "Mercury collective injection lost readiness during committed submission\n");
  }
  request.release();
}

void MercuryCollectiveAdapter::sendNotification(int vn)
{
  if (vn == config_.reduce_vn) notifyReadyIfPossible();
}

bool MercuryCollectiveAdapter::receiveResult(
    int vn, const SimpleNetwork::Request& request)
{
  if (activeInvocation() == 0 || vn != config_.result_vn) return false;
  const auto result = inspectStaticCollectiveResult(request,
      acceptedParticipant().route, activeInvocation(), config_.root_logical_nid,
      endpoint_logical_nid_, config_.result_vn);
  return result && completeSuccess(*result);
}

} // namespace SST::Hg
