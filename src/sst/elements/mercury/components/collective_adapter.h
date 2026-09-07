// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#pragma once

#include <merlin/services/collective/staticCollectiveEndpoint.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace SST::Hg {

class NIC;

/**
 * Narrow Mercury adapter for the static Merlin collective route.
 *
 * This deliberately supports one local participant, functional SUM/F64,
 * one element, one chunk, and one active invocation.  It owns no global
 * membership or tree description.
 */
class MercuryCollectiveAdapter final : public SST::Collective::StaticCollectiveEndpointBase
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
    uint64_t submit_delay_ns = 0;
    uint64_t completion_delay_ns = 0;
  };

  MercuryCollectiveAdapter(NIC& owner,
      SST::Interfaces::SimpleNetwork& network, StaticConfig config);

  /** True when the connected SimpleNetwork advertises the full static contract. */
  bool transportAvailable() const;

  /** Installs the one-rank endpoint route after SimpleNetwork setup. */
  bool installStaticRoute(
      SST::Interfaces::SimpleNetwork::nid_t physical_endpoint_id,
      SST::Interfaces::SimpleNetwork::nid_t participant_logical_id);

  /** Consumes a tag-first collective result packet. */
  bool receiveResult(int vn,
      const SST::Interfaces::SimpleNetwork::Request& request);

  /** Routes a typed SimpleNetwork send notification to ready progress. */
  void sendNotification(int vn);

private:
  bool transportReady(
      const SST::Collective::CollectiveSignatureV1& signature) const override;
  void commitContribution(
      SST::Collective::StaticCollectiveContribution&& contribution) noexcept override;
  void stageContribution();
  void contributionStaged();
  void sendContribution();
  void resultStaged();
  void finishResult();

  NIC& owner_;
  SST::Interfaces::SimpleNetwork& network_;
  StaticConfig config_;

  SST::Interfaces::SimpleNetwork::nid_t endpoint_logical_nid_ = -1;
  std::unique_ptr<SST::Interfaces::SimpleNetwork::Request> pending_request_;
  std::optional<SST::Collective::StaticCollectiveResult> pending_result_;
  bool staging_contribution_ = false;
  uint64_t staging_bytes_ = 0;
};

} // namespace SST::Hg
