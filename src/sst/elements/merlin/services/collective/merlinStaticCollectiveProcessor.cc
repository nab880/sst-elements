// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "sst_config.h"

#include "merlinStaticCollectiveProcessor.h"

#include <sst/core/output.h>
#include <sst/core/params.h>
#include <sst/elements/merlin/router.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SST::Collective {

bool
MerlinStaticCollectiveRouteProjection::valid() const
{
    const auto valid_port = [](uint32_t port) {
        return port <= static_cast<uint32_t>(std::numeric_limits<int>::max());
    };

    if ( root == parent_port.has_value() || (parent_port && !valid_port(*parent_port)) ||
         !subtree_representative.valid() || !root_representative.valid() ||
         (root && !(subtree_representative == root_representative)) ||
         (child_branches.empty() && local_endpoint_branches.empty()) ||
         child_branches.size() > UINT32_MAX || local_endpoint_branches.size() > UINT32_MAX ||
         child_branches.size() > UINT32_MAX - local_endpoint_branches.size() ) {
        return false;
    }

    std::unordered_set<uint32_t> ports;
    ports.reserve(child_branches.size() + local_endpoint_branches.size() + (parent_port ? 1 : 0));
    if ( parent_port ) ports.insert(*parent_port);
    const auto valid_branch = [&ports, &valid_port](const MerlinStaticCollectiveBranch& branch) {
        return valid_port(branch.port) && branch.representative.valid() && ports.insert(branch.port).second;
    };
    return std::all_of(child_branches.begin(), child_branches.end(), valid_branch) &&
           std::all_of(local_endpoint_branches.begin(), local_endpoint_branches.end(), valid_branch);
}

namespace {

#ifdef __FAST_MATH__
#error "Collective arithmetic requires strict floating-point semantics"
#endif

using Request = SST::Interfaces::SimpleNetwork::Request;
using nid_t   = SST::Interfaces::SimpleNetwork::nid_t;

constexpr int REDUCE_VN = 0;
constexpr int RESULT_VN = 1;
constexpr int REDUCE_VC = 0;
constexpr int RESULT_VC = 1;
constexpr uint64_t STATIC_FLIT_BITS = 64;
constexpr int STATIC_REQUEST_FLITS = static_cast<int>(
    (CollectiveServiceData::MODELED_REQUEST_BITS + STATIC_FLIT_BITS - 1) / STATIC_FLIT_BITS);
static_assert(sizeof(double) == 8, "CollectiveDatatype::F64 requires an eight-byte double");
static_assert(CollectiveServiceData::VALUE_BYTES == sizeof(double),
    "Static collective value must hold one F64");
static_assert(CollectiveServiceData::MODELED_REQUEST_BITS == 784,
    "Static collective transport contract requires 784 modeled bits");
static_assert(STATIC_REQUEST_FLITS == 13, "Static collective transport requires thirteen 64-bit flits");
static_assert(std::numeric_limits<double>::is_iec559,
    "CollectiveDatatype::F64 requires IEEE-754 arithmetic");
static_assert(std::numeric_limits<double>::radix == 2 && std::numeric_limits<double>::digits == 53,
    "CollectiveDatatype::F64 requires IEEE-754 binary64 precision");

constexpr uint64_t DIAGNOSTIC_INVALID_INGRESS = 1;
constexpr uint64_t DIAGNOSTIC_MISSING_DATA    = 2;
constexpr uint64_t DIAGNOSTIC_WRONG_SERVICE  = 3;
constexpr uint64_t DIAGNOSTIC_NONATOMIC      = 4;
constexpr uint64_t DIAGNOSTIC_MALFORMED_PACKET = 0x100;
constexpr uint64_t DIAGNOSTIC_WRONG_PORT      = 0x200;
constexpr uint64_t DIAGNOSTIC_WRONG_VC        = 0x201;
constexpr uint64_t DIAGNOSTIC_PROVENANCE      = 0x202;
constexpr uint64_t DIAGNOSTIC_UNSUPPORTED = 0x203;
constexpr uint64_t DIAGNOSTIC_DUPLICATE       = 0x204;
constexpr uint64_t DIAGNOSTIC_UNEXPECTED      = 0x205;
constexpr uint64_t DIAGNOSTIC_BUSY_KEY        = 0x300;
constexpr uint64_t DIAGNOSTIC_BUSY_EGRESS     = 0x301;

enum class EgressKind : uint8_t { UpwardAggregate = 1, Result = 2 };

struct PendingEgress
{
    SST::Merlin::NetworkServiceSyntheticPacket packet;
    EgressKind                                  kind = EgressKind::Result;
};

RouteIdV1
makeStaticRoute(const SST::Params& params)
{
    return { params.find<uint64_t>("job_namespace", 1), params.find<uint64_t>("route_id", 1) };
}

bool
loadBranches(const SST::Params& params, const char* ports_name, const char* nids_name,
    const char* logical_nids_name,
    std::vector<MerlinStaticCollectiveBranch>& branches)
{
    std::vector<int>     ports;
    std::vector<int64_t> nids;
    std::vector<int64_t> logical_nids;
    params.find_array<int>(ports_name, ports);
    params.find_array<int64_t>(nids_name, nids);
    params.find_array<int64_t>(logical_nids_name, logical_nids);
    if ( ports.size() != nids.size() || (!logical_nids.empty() && logical_nids.size() != nids.size()) ) {
        return false;
    }

    branches.reserve(ports.size());
    for ( size_t index = 0; index < ports.size(); ++index ) {
        const int64_t logical_nid = logical_nids.empty() ? nids[index] : logical_nids[index];
        if ( ports[index] < 0 || nids[index] < 0 || logical_nid < 0 ||
             static_cast<uint64_t>(ports[index]) > std::numeric_limits<uint32_t>::max() ) {
            return false;
        }
        branches.push_back({ static_cast<uint32_t>(ports[index]),
            MerlinStaticCollectiveRepresentative {
                static_cast<nid_t>(nids[index]), static_cast<nid_t>(logical_nid) } });
    }
    return true;
}

bool
makeStaticProjection(const SST::Params& params, MerlinStaticCollectiveRouteProjection& projection)
{
    projection.root = params.find<bool>("root", false);

    const int parent_port = params.find<int>("parent_port", -1);
    if ( projection.root ) {
        if ( parent_port != -1 ) return false;
    }
    else {
        if ( parent_port < 0 ) return false;
        projection.parent_port = static_cast<uint32_t>(parent_port);
    }

    if ( !loadBranches(params, "child_ports", "child_nids", "child_logical_nids",
             projection.child_branches) ||
         !loadBranches(params, "local_ports", "local_nids", "local_logical_nids",
             projection.local_endpoint_branches) ) {
        return false;
    }

    const int64_t root_nid = params.find<int64_t>("root_nid", 0);
    int64_t root_logical_nid = params.find<int64_t>("root_logical_nid", -1);
    int64_t subtree_nid    = params.find<int64_t>("subtree_nid", -1);
    int64_t subtree_logical_nid = params.find<int64_t>("subtree_logical_nid", -1);
    if ( root_nid < 0 ) return false;
    if ( root_logical_nid < 0 ) root_logical_nid = root_nid;
    if ( subtree_nid < 0 ) {
        if ( !projection.local_endpoint_branches.empty() ) {
            subtree_nid = projection.local_endpoint_branches.front().representative.physical_endpoint_id;
            if ( subtree_logical_nid < 0 ) {
                subtree_logical_nid = projection.local_endpoint_branches.front().representative.caller_visible_logical_nid;
            }
        }
        else if ( !projection.child_branches.empty() ) {
            subtree_nid = projection.child_branches.front().representative.physical_endpoint_id;
            if ( subtree_logical_nid < 0 ) {
                subtree_logical_nid = projection.child_branches.front().representative.caller_visible_logical_nid;
            }
        }
    }
    if ( subtree_nid < 0 ) return false;
    if ( subtree_logical_nid < 0 ) subtree_logical_nid = subtree_nid;

    projection.root_representative =
        { static_cast<nid_t>(root_nid), static_cast<nid_t>(root_logical_nid) };
    projection.subtree_representative =
        { static_cast<nid_t>(subtree_nid), static_cast<nid_t>(subtree_logical_nid) };
    return true;
}

double
decodeValue(const CollectiveServiceData& data)
{
    double value = 0.0;
    std::memcpy(&value, data.value.data(), sizeof(value));
    return value;
}

std::array<uint8_t, CollectiveServiceData::VALUE_BYTES>
encodeValue(double value)
{
    std::array<uint8_t, CollectiveServiceData::VALUE_BYTES> bytes {};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

} // namespace

class MerlinStaticCollectiveProcessor::Impl
{
public:
    enum class Phase : uint8_t { Empty = 0, Collecting = 1, AwaitingResult = 2, FanoutResult = 3 };

    struct Branch
    {
        uint32_t                                     port = 0;
        MerlinStaticCollectiveRepresentative representative;
        bool                                         local = false;
    };

    struct ActiveState
    {
        Phase                phase = Phase::Empty;
        uint64_t             invocation_id = 0;
        std::vector<double>  values;
        std::vector<uint8_t> arrived;
        uint32_t             arrival_count = 0;
    };

    struct Statistics
    {
        SST::Statistics::Statistic<uint64_t>* local_contributions = nullptr;
        SST::Statistics::Statistic<uint64_t>* child_contributions = nullptr;
        SST::Statistics::Statistic<uint64_t>* parent_results = nullptr;
        SST::Statistics::Statistic<uint64_t>* upward_aggregates = nullptr;
        SST::Statistics::Statistic<uint64_t>* result_packets = nullptr;
        SST::Statistics::Statistic<uint64_t>* active_high_water = nullptr;
        SST::Statistics::Statistic<uint64_t>* installed_branch_slots = nullptr;
        SST::Statistics::Statistic<uint64_t>* egress_retries = nullptr;
    };

    Impl(SST::Merlin::NetworkServiceHost* host, uint32_t egress_capacity) :
        host(host),
        egress_slots(egress_capacity)
    {}

    bool outputsSupported(const MerlinStaticCollectiveRouteProjection& candidate_projection) const
    {
        if ( host == nullptr ) return false;
        const auto output_supported = [this](int route_vn, uint32_t port, int vc) {
            return host->supportsNetworkServiceOutput(
                { route_vn, static_cast<int>(port), vc,
                    static_cast<size_t>(CollectiveServiceData::MODELED_REQUEST_BITS), STATIC_REQUEST_FLITS });
        };
        if ( !candidate_projection.root &&
             !output_supported(REDUCE_VN, *candidate_projection.parent_port, REDUCE_VC) ) {
            return false;
        }
        for ( const auto& branch : candidate_projection.child_branches ) {
            if ( !output_supported(RESULT_VN, branch.port, RESULT_VC) ) {
                return false;
            }
        }
        for ( const auto& branch : candidate_projection.local_endpoint_branches ) {
            if ( !output_supported(RESULT_VN, branch.port, RESULT_VC) ) {
                return false;
            }
        }
        return true;
    }

    bool transportSupported() const { return installed && outputsSupported(projection); }

    bool install(RouteIdV1 offered_route, MerlinStaticCollectiveRouteProjection&& offered_projection)
    {
        if ( host == nullptr || egress_slots.empty() || !offered_route.valid() ||
             !offered_projection.valid() ) {
            return false;
        }

        const size_t branch_count = offered_projection.child_branches.size() +
                                    offered_projection.local_endpoint_branches.size();
        if ( branch_count == 0 || branch_count > egress_slots.size() || branch_count > UINT32_MAX ) {
            return false;
        }

        if ( !outputsSupported(offered_projection) ) {
            return false;
        }

        route      = offered_route;
        projection = std::move(offered_projection);
        branches.reserve(branch_count);
        branch_by_port.reserve(branch_count);
        for ( const auto& branch : projection.child_branches ) {
            branch_by_port.emplace(branch.port, branches.size());
            branches.push_back({ branch.port, branch.representative, false });
        }
        for ( const auto& branch : projection.local_endpoint_branches ) {
            branch_by_port.emplace(branch.port, branches.size());
            branches.push_back({ branch.port, branch.representative, true });
        }
        active.values.assign(branch_count, 0.0);
        active.arrived.assign(branch_count, 0);
        installed = true;

        add(statistics.installed_branch_slots, branch_count);
        return true;
    }

    SST::Merlin::NetworkServiceDecision inspect(int input_port, int input_vc,
        const SST::Merlin::internal_router_event* event) const;
    void consume(int input_port, int input_vc,
        const SST::Merlin::internal_router_event& event) noexcept;

    bool progress()
    {
        while ( egress_count != 0 ) {
            PendingEgress& pending = egress_slots[egress_head];
            if ( !host->tryEnqueueNetworkServiceOutput(CollectiveServiceData::SERVICE_ID, pending.packet) ) {
                add(statistics.egress_retries, 1);
                return false;
            }

            if ( pending.kind == EgressKind::UpwardAggregate ) {
                add(statistics.upward_aggregates, 1);
            }
            else {
                add(statistics.result_packets, 1);
            }

            pending = PendingEgress {};
            egress_head = (egress_head + 1) % egress_slots.size();
            --egress_count;
        }

        if ( active.phase == Phase::FanoutResult ) {
            retired_invocation_id = active.invocation_id;
            has_retired_invocation = true;
            clearActive();
        }
        return true;
    }

    size_t findBranch(int port) const
    {
        const auto found = branch_by_port.find(static_cast<uint32_t>(port));
        return found == branch_by_port.end() ? branches.size() : found->second;
    }

    void clearActive() noexcept
    {
        active.phase         = Phase::Empty;
        active.invocation_id = 0;
        active.arrival_count = 0;
        std::fill(active.values.begin(), active.values.end(), 0.0);
        std::fill(active.arrived.begin(), active.arrived.end(), uint8_t { 0 });
    }

    static constexpr SST::Merlin::NetworkServiceDecision reject(uint64_t diagnostic)
    {
        return { SST::Merlin::NetworkServiceDisposition::Reject, diagnostic };
    }

    static void add(SST::Statistics::Statistic<uint64_t>* statistic, uint64_t value)
    {
        if ( statistic != nullptr ) statistic->addData(value);
    }

    SST::Merlin::NetworkServiceHost* host = nullptr;
    bool installed = false;
    RouteIdV1 route;
    MerlinStaticCollectiveRouteProjection projection;
    std::vector<Branch> branches;
    std::unordered_map<uint32_t, size_t> branch_by_port;
    ActiveState active;
    uint64_t retired_invocation_id = 0;
    bool has_retired_invocation = false;
    std::vector<PendingEgress> egress_slots;
    size_t egress_head  = 0;
    size_t egress_count = 0;
    bool active_high_water_reported = false;
    Statistics statistics;

private:
    struct IngressFacts
    {
        uint64_t invocation_id = 0;
        size_t   branch_index  = 0;
        double   value         = 0.0;
        bool     contribution  = false;
    };

    SST::Merlin::NetworkServiceDecision inspect(int input_port, int input_vc,
        const SST::Merlin::internal_router_event* event, IngressFacts* facts) const;

    void appendEgress(PendingEgress&& packet) noexcept
    {
        const size_t tail = (egress_head + egress_count) % egress_slots.size();
        egress_slots[tail] = std::move(packet);
        ++egress_count;
    }

    PendingEgress makeEgress(uint64_t invocation_id, double value, CollectiveDirection direction,
        const MerlinStaticCollectiveRepresentative& source,
        const MerlinStaticCollectiveRepresentative& destination, uint32_t output_port) const
    {
        const bool result = direction == CollectiveDirection::Result;
        const int vn = result ? RESULT_VN : REDUCE_VN;
        auto request = std::make_unique<Request>(destination.physical_endpoint_id,
            source.caller_visible_logical_nid,
            static_cast<size_t>(CollectiveServiceData::MODELED_REQUEST_BITS), true, true);
        request->vn             = vn;
        request->allow_adaptive = false;
        auto data = std::make_unique<CollectiveServiceData>(route, invocation_id, direction, encodeValue(value));
        request->giveServiceData(data.get());
        data.release();

        PendingEgress pending;
        pending.kind                    = result ? EgressKind::Result : EgressKind::UpwardAggregate;
        pending.packet.request          = std::move(request);
        pending.packet.trusted_src      = source.physical_endpoint_id;
        pending.packet.route_vn         = vn;
        pending.packet.output_port      = static_cast<int>(output_port);
        pending.packet.output_vc        = vn;
        pending.packet.size_in_flits    = STATIC_REQUEST_FLITS;
        return pending;
    }

    friend class MerlinStaticCollectiveProcessor;
};

SST::Merlin::NetworkServiceDecision
MerlinStaticCollectiveProcessor::Impl::inspect(
    int input_port, int input_vc, const SST::Merlin::internal_router_event* event) const
{
    return inspect(input_port, input_vc, event, nullptr);
}

SST::Merlin::NetworkServiceDecision
MerlinStaticCollectiveProcessor::Impl::inspect(int input_port, int input_vc,
    const SST::Merlin::internal_router_event* event, IngressFacts* facts) const
{
    using SST::Merlin::NetworkServiceDisposition;

    if ( !installed || event == nullptr || input_port < 0 || input_vc < 0 ) {
        return reject(DIAGNOSTIC_INVALID_INGRESS);
    }

    const Request* request = event->inspectRequest();
    if ( request == nullptr ) {
        return reject(DIAGNOSTIC_MISSING_DATA);
    }
    if ( request->getServiceID() != CollectiveServiceData::SERVICE_ID ) {
        return reject(DIAGNOSTIC_WRONG_SERVICE);
    }
    if ( request->inspectPayload() != nullptr || !request->head || !request->tail || request->allow_adaptive ) {
        return reject(DIAGNOSTIC_NONATOMIC);
    }
    const CollectiveServiceData* data = request->inspectServiceDataAs<CollectiveServiceData>();
    if ( data == nullptr ) return reject(DIAGNOSTIC_MISSING_DATA);

    const bool contribution = data->direction == CollectiveDirection::Contribution;
    const bool result       = data->direction == CollectiveDirection::Result;
    size_t branch_index     = branches.size();
    const MerlinStaticCollectiveRepresentative* expected_source = nullptr;
    nid_t expected_destination = -1;
    int expected_vc = -1;
    int expected_logical_vn = -1;

    if ( contribution ) {
        branch_index = findBranch(input_port);
        if ( branch_index == branches.size() ) return reject(DIAGNOSTIC_WRONG_PORT);
        expected_source      = &branches[branch_index].representative;
        expected_destination = projection.root_representative.physical_endpoint_id;
        expected_vc          = REDUCE_VC;
        expected_logical_vn  = REDUCE_VN;
    }
    else if ( result ) {
        if ( projection.root || !projection.parent_port ||
             input_port != static_cast<int>(*projection.parent_port) ) {
            return reject(DIAGNOSTIC_WRONG_PORT);
        }
        expected_source       = &projection.root_representative;
        expected_destination  = projection.subtree_representative.physical_endpoint_id;
        expected_vc           = RESULT_VC;
        expected_logical_vn   = RESULT_VN;
    }
    else {
        return reject(DIAGNOSTIC_UNSUPPORTED);
    }

    const CollectiveDirection expected_direction = contribution ?
        CollectiveDirection::Contribution : CollectiveDirection::Result;
    if ( !data->validFor(route, expected_direction, request->size_in_bits) ) {
        return reject(DIAGNOSTIC_MALFORMED_PACKET);
    }
    if ( input_vc != expected_vc || request->vn != expected_logical_vn ) {
        return reject(DIAGNOSTIC_WRONG_VC);
    }
    if ( request->src != expected_source->caller_visible_logical_nid ||
         event->getSrc() != expected_source->physical_endpoint_id ||
         request->dest != expected_destination ) {
        return reject(DIAGNOSTIC_PROVENANCE);
    }
    if ( has_retired_invocation && data->invocation_id <= retired_invocation_id ) {
        return reject(DIAGNOSTIC_DUPLICATE);
    }

    if ( active.phase != Phase::Empty && active.invocation_id != data->invocation_id ) {
        return { NetworkServiceDisposition::Busy, DIAGNOSTIC_BUSY_KEY };
    }

    if ( contribution ) {
        if ( active.phase != Phase::Empty && active.phase != Phase::Collecting ) {
            return reject(DIAGNOSTIC_UNEXPECTED);
        }
        if ( active.phase == Phase::Collecting && active.arrived[branch_index] != 0 ) {
            return reject(DIAGNOSTIC_DUPLICATE);
        }

        const uint32_t arrivals_after_commit = active.arrival_count + 1;
        if ( arrivals_after_commit == branches.size() ) {
            const size_t required_outputs = projection.root ? branches.size() : 1;
            if ( egress_count + required_outputs > egress_slots.size() ) {
                return { NetworkServiceDisposition::Busy, DIAGNOSTIC_BUSY_EGRESS };
            }

            for ( size_t index = 0; index < branches.size(); ++index ) {
                if ( index != branch_index && active.arrived[index] == 0 ) {
                    return reject(DIAGNOSTIC_UNEXPECTED);
                }
            }
        }
    }
    else {
        if ( active.phase != Phase::AwaitingResult ) {
            return reject(DIAGNOSTIC_UNEXPECTED);
        }
        if ( egress_count + branches.size() > egress_slots.size() ) {
            return { NetworkServiceDisposition::Busy, DIAGNOSTIC_BUSY_EGRESS };
        }
    }

    if ( facts != nullptr ) {
        facts->invocation_id = data->invocation_id;
        facts->branch_index  = branch_index;
        facts->value         = decodeValue(*data);
        facts->contribution  = contribution;
    }
    return { NetworkServiceDisposition::Accept, 0 };
}

void
MerlinStaticCollectiveProcessor::Impl::consume(
    int input_port, int input_vc, const SST::Merlin::internal_router_event& event) noexcept
{
    IngressFacts facts;
    if ( inspect(input_port, input_vc, &event, &facts).disposition !=
         SST::Merlin::NetworkServiceDisposition::Accept ) {
        std::terminate();
    }

    if ( facts.contribution ) {
        const bool starts_active = active.phase == Phase::Empty;
        const bool completes_reduction = active.arrival_count + 1 == branches.size();
        double reduced = facts.value;
        if ( completes_reduction ) {
            for ( size_t index = 0; index < branches.size(); ++index ) {
                const double value = index == facts.branch_index ? facts.value : active.values[index];
                if ( index == 0 ) {
                    reduced = value;
                }
                else {
                    volatile double rounded = reduced + value;
                    reduced = rounded;
                }
            }
        }

        if ( starts_active ) {
            clearActive();
            active.phase         = Phase::Collecting;
            active.invocation_id = facts.invocation_id;
            if ( !active_high_water_reported ) {
                active_high_water_reported = true;
                add(statistics.active_high_water, 1);
            }
        }

        active.values[facts.branch_index]  = facts.value;
        active.arrived[facts.branch_index] = 1;
        ++active.arrival_count;
        add(branches[facts.branch_index].local ? statistics.local_contributions :
                                                 statistics.child_contributions,
            1);

        if ( completes_reduction ) {
            active.phase  = projection.root ? Phase::FanoutResult : Phase::AwaitingResult;
            if ( projection.root ) {
                for ( const Branch& branch : branches ) {
                    appendEgress(makeEgress(facts.invocation_id, reduced, CollectiveDirection::Result,
                        projection.root_representative, branch.representative, branch.port));
                }
            }
            else {
                appendEgress(makeEgress(facts.invocation_id, reduced,
                    CollectiveDirection::Contribution, projection.subtree_representative,
                    projection.root_representative, *projection.parent_port));
            }
        }
    }
    else {
        active.phase  = Phase::FanoutResult;
        add(statistics.parent_results, 1);
        for ( const Branch& branch : branches ) {
            appendEgress(makeEgress(facts.invocation_id, facts.value, CollectiveDirection::Result,
                projection.root_representative, branch.representative, branch.port));
        }
    }
}

MerlinStaticCollectiveProcessor::MerlinStaticCollectiveProcessor(
    SST::ComponentId_t id, SST::Params& params, SST::Merlin::NetworkServiceHost* host) :
    SST::Merlin::NetworkServiceProcessor(id),
    impl_(std::make_unique<Impl>(host, params.find<uint32_t>("pending_egress_capacity", 8)))
{
    impl_->statistics.local_contributions = registerStatistic<uint64_t>("local_contributions");
    impl_->statistics.child_contributions = registerStatistic<uint64_t>("child_contributions");
    impl_->statistics.parent_results = registerStatistic<uint64_t>("parent_results");
    impl_->statistics.upward_aggregates = registerStatistic<uint64_t>("upward_aggregates");
    impl_->statistics.result_packets = registerStatistic<uint64_t>("result_packets");
    impl_->statistics.active_high_water = registerStatistic<uint64_t>("active_high_water");
    impl_->statistics.installed_branch_slots = registerStatistic<uint64_t>("installed_branch_slots");
    impl_->statistics.egress_retries = registerStatistic<uint64_t>("egress_retries");

    const RouteIdV1 route = makeStaticRoute(params);
    MerlinStaticCollectiveRouteProjection projection;
    if ( !makeStaticProjection(params, projection) || !impl_->install(route, std::move(projection)) ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "merlin.collective_static_processor received an invalid or unsupported static local projection\n");
    }

    egress_handler_ = new SST::Clock::Handler<MerlinStaticCollectiveProcessor,
        &MerlinStaticCollectiveProcessor::egressTick>(this);
    egress_clock_ = registerClock(params.find<std::string>("egress_clock", "1GHz"), egress_handler_, false);
}

MerlinStaticCollectiveProcessor::MerlinStaticCollectiveProcessor(SST::Merlin::NetworkServiceHost* host,
    RouteIdV1 route, MerlinStaticCollectiveRouteProjection local_projection,
    uint32_t pending_egress_capacity) :
    SST::Merlin::NetworkServiceProcessor(),
    impl_(std::make_unique<Impl>(host, pending_egress_capacity))
{
    if ( !impl_->install(route, std::move(local_projection)) ) {
        throw std::invalid_argument("Invalid static Merlin collective test projection");
    }
}

MerlinStaticCollectiveProcessor::~MerlinStaticCollectiveProcessor() = default;

SST::Merlin::NetworkServiceDecision
MerlinStaticCollectiveProcessor::inspect(const SST::Merlin::NetworkServiceIngress& ingress) const
{
    return impl_->inspect(ingress.input_port, ingress.input_vc, ingress.event);
}

void
MerlinStaticCollectiveProcessor::consume(
    SST::Merlin::NetworkServiceOwnedIngress ingress) noexcept
{
    auto event = std::move(ingress.event);
    if ( event == nullptr ) std::terminate();
    impl_->consume(ingress.input_port, ingress.input_vc, *event);
    ensureEgressProgress();
}

bool
MerlinStaticCollectiveProcessor::hasScheduledWork() const
{
    return impl_ != nullptr && impl_->egress_count != 0;
}

bool
MerlinStaticCollectiveProcessor::validateInstalledTransport() const
{
    return impl_ != nullptr && impl_->transportSupported();
}

bool
MerlinStaticCollectiveProcessor::progressPendingEgress()
{
    return impl_->progress();
}

bool
MerlinStaticCollectiveProcessor::hasActiveInvocation() const
{
    return impl_ != nullptr && impl_->active.phase != Impl::Phase::Empty;
}

uint32_t
MerlinStaticCollectiveProcessor::pendingEgressCount() const
{
    return impl_ == nullptr ? 0 : static_cast<uint32_t>(impl_->egress_count);
}

bool
MerlinStaticCollectiveProcessor::egressTick(SST::Cycle_t cycle)
{
    (void)cycle;
    return progressPendingEgress();
}

void
MerlinStaticCollectiveProcessor::ensureEgressProgress() noexcept
{
    if ( impl_->egress_count == 0 ) return;
    if ( egress_handler_ != nullptr && !egress_handler_->isActive() ) {
        reregisterClock(egress_clock_, egress_handler_);
    }
    impl_->host->wakeNetworkServiceProcessor();
}

} // namespace SST::Collective
