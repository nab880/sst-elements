// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#pragma once

#include <sst/elements/merlin/services/collective/collectiveEndpoint.h>
#include <sst/elements/merlin/services/collective/collectiveServiceData.h>

#include <cstdint>
#include <optional>

namespace SST::Hg {

class NIC;

/**
 * Narrow Mercury adapter for the experimental static Merlin route.
 *
 * This deliberately supports one local participant, functional SUM/F64,
 * one element, one chunk, and one active invocation.  It owns no global
 * membership or tree description.
 */
class MercuryCollectiveAdapter final : public SST::Collective::CollectiveEndpoint
{
public:
  struct StaticConfig
  {
    uint64_t job_namespace = 1;
    uint64_t route_id = 1;
    SST::Interfaces::SimpleNetwork::nid_t root_physical_nid = 0;
    SST::Interfaces::SimpleNetwork::nid_t root_logical_nid = 0;
    int reduce_vn = -1;
    int result_vn = -1;
  };

  MercuryCollectiveAdapter(NIC& owner,
      SST::Interfaces::SimpleNetwork& network, uint64_t owner_component_id,
      StaticConfig config);

  /** True when the connected SimpleNetwork advertises the full POC contract. */
  bool transportAvailable() const;

  /** Installs the one-rank endpoint route after SimpleNetwork setup. */
  bool installStaticRoute(
      SST::Interfaces::SimpleNetwork::nid_t physical_endpoint_id,
      SST::Interfaces::SimpleNetwork::nid_t participant_logical_id);

  bool supportsCollective(
      const SST::Collective::CollectiveSignatureV1& signature) const override;

  bool bindParticipant(
      const SST::Collective::AcceptedParticipantHandle& participant,
      SST::Collective::CollectiveCompletionSink& completion,
      SST::Collective::CollectiveReadySink& ready) override;

  SST::Collective::CollectiveSubmitResult trySubmitCollective(
      SST::Collective::CollectivePending& pending) override;

  void requestCollectiveReady(
      const SST::Collective::AcceptedParticipantHandle& participant,
      const SST::Collective::CollectiveSignatureV1& signature) override;

  const SST::Collective::AcceptedParticipantHandle* participant(
      uint32_t local_slot) const;

  /** Consumes a tag-first collective result packet. */
  bool receiveResult(int vn,
      const SST::Interfaces::SimpleNetwork::Request& request);

  /** Routes a typed SimpleNetwork send notification to ready progress. */
  void sendNotification(int vn);

  bool quiescent() const;

private:
  void notifyReadyIfPossible();

  NIC& owner_;
  SST::Interfaces::SimpleNetwork& network_;
  uint64_t owner_component_id_;
  StaticConfig config_;

  SST::Collective::RouteIdV1 route_;
  SST::Collective::AcceptedParticipantHandle accepted_;
  SST::Interfaces::SimpleNetwork::nid_t endpoint_logical_nid_ = -1;
  bool installed_ = false;

  SST::Collective::CollectiveCompletionSink* completion_ = nullptr;
  SST::Collective::CollectiveReadySink* ready_ = nullptr;
  std::optional<SST::Collective::CollectiveCompletionToken> token_;
  SST::Collective::MutableBufferView result_;
  SST::Collective::CollectiveSignatureV1 active_signature_;
  SST::Collective::CollectiveSignatureV1 ready_signature_;
  uint64_t active_invocation_ = 0;
  uint64_t completed_invocation_ = 0;
  bool active_ = false;
  bool ready_armed_ = false;
};

} // namespace SST::Hg
