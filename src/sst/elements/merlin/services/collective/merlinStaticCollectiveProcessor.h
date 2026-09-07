// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#ifndef SST_ELEMENTS_COLLECTIVE_MERLIN_STATIC_COLLECTIVE_PROCESSOR_H
#define SST_ELEMENTS_COLLECTIVE_MERLIN_STATIC_COLLECTIVE_PROCESSOR_H

#include "collectiveServiceData.h"

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

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(physical_endpoint_id);
        SST_SER(caller_visible_logical_nid);
    }
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
    uint32_t                             port = 0;
    MerlinStaticCollectiveRepresentative representative;

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(port);
        representative.serialize_order(ser);
    }
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
    void serialize_order(SST::Core::Serialization::serializer& ser);
};

/**
 * Experimental one-route Merlin collective processor.
 *
 * This intentionally implements only the proof-of-concept subset:
 * functional SUM/F64, one element, one chunk, one active invocation, one
 * statically installed fat-tree projection, and lazy result fanout.  Its
 * ordered scalar reduction has configurable operations per cycle and result
 * latency.  It owns the reduce and result VNs named by its parameters; every
 * head on them is offered to inspect().  Generated packets enter Merlin
 * through the router's bounded synthetic requester and normal crossbar
 * arbitration, driven by the router clock.
 *
 * The processor checkpoints its static configuration and quiescent state;
 * an active invocation or pending egress at checkpoint time is rejected.
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
        { "reduce_vn", "VN carrying contributions toward the root; the processor owns it on this router", "0" },
        { "result_vn", "VN carrying results toward the leaves; the processor owns it on this router", "1" },
        { "reduction_ops_per_cycle", "Maximum ordered scalar additions per router clock cycle", "1" },
        { "reduction_latency_cycles", "Router clock cycles from the final addition to result readiness", "1" }
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
        int reduce_vn = 0, int result_vn = 1,
        uint32_t reduction_ops_per_cycle = 1, uint32_t reduction_latency_cycles = 1);

    MerlinStaticCollectiveProcessor();
    ~MerlinStaticCollectiveProcessor() override;

    SST::Merlin::NetworkServiceID getServiceID() const override { return CollectiveServiceData::SERVICE_ID; }
    SST::Merlin::NetworkServiceRequestContract getRequestContract() const override
    {
        return { CollectiveServiceData::SERVICE_ID, CollectiveServiceData::DATA_TOKEN,
            CollectiveServiceData::MIN_SCHEMA_VERSION, CollectiveServiceData::MAX_SCHEMA_VERSION };
    }
    std::vector<int> ownedVNs() const override;
    bool validateInstalledTransport() const override;
    SST::Merlin::NetworkServiceDecision inspect(
        const SST::Merlin::NetworkServiceIngress& ingress) const override;
    void consume(SST::Merlin::NetworkServiceOwnedIngress ingress) noexcept override;
    bool hasScheduledWork() const override;
    bool progress() override;

    void serialize_order(SST::Core::Serialization::serializer& ser) override;
    ImplementSerializable(SST::Collective::MerlinStaticCollectiveProcessor)

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SST::Collective

#endif // SST_ELEMENTS_COLLECTIVE_MERLIN_STATIC_COLLECTIVE_PROCESSOR_H
