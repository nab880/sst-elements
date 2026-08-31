// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_COLLECTIVE_MERLIN_STATIC_COLLECTIVE_PROCESSOR_H
#define SST_ELEMENTS_COLLECTIVE_MERLIN_STATIC_COLLECTIVE_PROCESSOR_H

#include "collectiveServiceData.h"

#include <sst/core/clock.h>
#include <sst/core/timeConverter.h>
#include "../../networkService.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace SST::Collective {

/** Processor-local transport identity used by the fixed Merlin proof. */
struct MerlinStaticCollectiveRepresentative
{
    SimpleNetwork::nid_t physical_endpoint_id       = -1;
    SimpleNetwork::nid_t caller_visible_logical_nid = -1;

    constexpr bool valid() const { return physical_endpoint_id >= 0 && caller_visible_logical_nid >= 0; }
};

inline constexpr bool
operator==(const MerlinStaticCollectiveRepresentative& lhs,
    const MerlinStaticCollectiveRepresentative& rhs)
{
    return lhs.physical_endpoint_id == rhs.physical_endpoint_id &&
           lhs.caller_visible_logical_nid == rhs.caller_visible_logical_nid;
}

struct MerlinStaticCollectiveBranch
{
    uint32_t                                      port = 0;
    MerlinStaticCollectiveRepresentative representative;
};

/** Local tree facts consumed directly by the fixed processor and its tests. */
struct MerlinStaticCollectiveRouteProjection
{
    bool                                      root = false;
    std::optional<uint32_t>                   parent_port;
    MerlinStaticCollectiveRepresentative      subtree_representative;
    MerlinStaticCollectiveRepresentative      root_representative;
    std::vector<MerlinStaticCollectiveBranch> child_branches;
    std::vector<MerlinStaticCollectiveBranch> local_endpoint_branches;

    bool valid() const;
};

/**
 * Experimental one-route Merlin collective processor.
 *
 * This intentionally implements only the PR3 proof-of-concept subset:
 * functional SUM/F64, one element, one chunk, one active invocation, one
 * statically installed fat-tree projection, and a bounded pending-egress
 * ring.  Every generated packet still enters Merlin through the generic
 * bounded synthetic requester and normal crossbar arbitration.
 */
class MerlinStaticCollectiveProcessor final : public SST::Merlin::NetworkServiceProcessor
{
public:
    SST_ELI_REGISTER_SUBCOMPONENT(MerlinStaticCollectiveProcessor, "merlin", "collective_static_processor",
        SST_ELI_ELEMENT_VERSION(1, 0, 0), "Experimental static Merlin SUM/F64 collective processor",
        SST::Merlin::NetworkServiceProcessor)

    SST_ELI_DOCUMENT_PARAMS(
        { "root", "Whether this router is the static collective tree root", "false" },
        { "parent_port", "Parent output/input port, or -1 at the root", "-1" },
        { "child_ports", "Ordered child-router ports", "" },
        { "child_nids", "Representative physical NIDs parallel to child_ports", "" },
        { "child_logical_nids", "Optional caller-visible logical NIDs parallel to child_ports", "" },
        { "local_ports", "Ordered local-endpoint ports", "" },
        { "local_nids", "Representative physical NIDs parallel to local_ports", "" },
        { "local_logical_nids", "Optional caller-visible logical NIDs parallel to local_ports", "" },
        { "root_nid", "Physical representative NID of the root subtree", "0" },
        { "root_logical_nid", "Caller-visible logical NID of the root; negative defaults to root_nid", "-1" },
        { "subtree_nid", "This subtree physical representative; negative derives it from the first local/child branch",
            "-1" },
        { "subtree_logical_nid", "This subtree logical representative; negative derives/defaults from its physical representative", "-1" },
        { "job_namespace", "Static route job namespace", "1" },
        { "route_id", "Static route identifier", "1" },
        { "pending_egress_capacity", "Fixed processor-side pending synthetic packet capacity", "8" },
        { "egress_clock", "On-demand retry clock for pending synthetic packets", "1GHz" }
    )

    SST_ELI_DOCUMENT_STATISTICS(
        { "local_contributions", "Accepted contributions from local endpoint branches", "packets", 1 },
        { "child_contributions", "Accepted aggregate contributions from child routers", "packets", 1 },
        { "parent_results", "Accepted results from the parent router", "packets", 1 },
        { "upward_aggregates", "Aggregate packets admitted to Merlin synthetic arbitration", "packets", 1 },
        { "result_packets", "Result packets admitted to Merlin synthetic arbitration", "packets", 1 },
        { "active_high_water", "Maximum simultaneously active invocation keys", "keys", 1 },
        { "installed_branch_slots", "Branch-state slots allocated by the static local projection", "slots", 1 },
        { "egress_retries", "Processor egress attempts rejected by the bounded Merlin requester", "attempts", 1 }
    )

    MerlinStaticCollectiveProcessor(
        SST::ComponentId_t id, SST::Params& params, SST::Merlin::NetworkServiceHost* host);

    /** Deterministic constructor used by focused contract fixtures. */
    MerlinStaticCollectiveProcessor(SST::Merlin::NetworkServiceHost* host,
        RouteIdV1 route, MerlinStaticCollectiveRouteProjection local_projection,
        uint32_t pending_egress_capacity);

    ~MerlinStaticCollectiveProcessor() override;

    SST::Merlin::NetworkServiceID getServiceID() const override { return CollectiveServiceData::SERVICE_ID; }
    SST::Merlin::NetworkServiceRequestContract getRequestContract() const override
    {
        return { CollectiveServiceData::SERVICE_ID, CollectiveServiceData::DATA_TOKEN,
            CollectiveServiceData::MIN_SCHEMA_VERSION, CollectiveServiceData::MAX_SCHEMA_VERSION };
    }
    bool validateInstalledTransport() const override;
    SST::Merlin::NetworkServiceDecision inspect(
        const SST::Merlin::NetworkServiceIngress& ingress) const override;
    void consume(SST::Merlin::NetworkServiceOwnedIngress ingress) noexcept override;
    bool hasScheduledWork() const override;

    /** Attempts pending outputs in FIFO order; returns true only when the fixed ring drains. */
    bool progressPendingEgress();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    SST::TimeConverter    egress_clock_;
    SST::Clock::HandlerBase* egress_handler_ = nullptr;

    bool egressTick(SST::Cycle_t cycle);
    void ensureEgressProgress() noexcept;
};

} // namespace SST::Collective

#endif // SST_ELEMENTS_COLLECTIVE_MERLIN_STATIC_COLLECTIVE_PROCESSOR_H
