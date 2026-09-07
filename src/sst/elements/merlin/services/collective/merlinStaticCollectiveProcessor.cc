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

template <class T>
void
serializeVector(SST::Core::Serialization::serializer& ser, std::vector<T>& values)
{
    size_t count = values.size();
    SST_SER(count);
    if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) values.resize(count);
    for ( auto& value : values ) value.serialize_order(ser);
}

} // namespace

void
MerlinStaticCollectiveRouteProjection::serialize_order(SST::Core::Serialization::serializer& ser)
{
    SST_SER(root);
    bool     has_parent  = parent_port.has_value();
    uint32_t parent_value = parent_port.value_or(0);
    SST_SER(has_parent);
    SST_SER(parent_value);
    if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
        if ( has_parent ) parent_port = parent_value;
        else parent_port.reset();
    }
    subtree_representative.serialize_order(ser);
    root_representative.serialize_order(ser);
    serializeVector(ser, child_branches);
    serializeVector(ser, local_endpoint_branches);
}

namespace {

#ifdef __FAST_MATH__
#error "Collective arithmetic requires strict floating-point semantics"
#endif

using Request = SST::Interfaces::SimpleNetwork::Request;
using nid_t   = SST::Interfaces::SimpleNetwork::nid_t;

static_assert(sizeof(double) == 8, "CollectiveDatatype::F64 requires an eight-byte double");
static_assert(STATIC_COLLECTIVE_SIGNATURE_V1.payloadBytes() == sizeof(double),
    "Static collective value must hold one F64");
static_assert(STATIC_COLLECTIVE_REQUEST_BITS == 784,
    "Static collective transport contract requires 784 modeled bits");
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
constexpr uint64_t DIAGNOSTIC_WRONG_VN        = 0x201;
constexpr uint64_t DIAGNOSTIC_PROVENANCE      = 0x202;
constexpr uint64_t DIAGNOSTIC_UNSUPPORTED = 0x203;
constexpr uint64_t DIAGNOSTIC_DUPLICATE       = 0x204;
constexpr uint64_t DIAGNOSTIC_UNEXPECTED      = 0x205;
constexpr uint64_t DIAGNOSTIC_BUSY_KEY        = 0x300;
constexpr uint64_t DIAGNOSTIC_BUSY_REDUCTION  = 0x301;

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

std::vector<uint8_t>
encodeValue(double value)
{
    std::vector<uint8_t> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

} // namespace

class MerlinStaticCollectiveProcessor::Impl
{
public:
    enum class Phase : uint8_t {
        Empty = 0, Collecting = 1, Reducing = 2, Upward = 3, AwaitingResult = 4, FanoutResult = 5
    };

    struct Branch
    {
        uint32_t                             port = 0;
        MerlinStaticCollectiveRepresentative representative;
        bool                                 local = false;
    };

    struct ActiveState
    {
        Phase                phase = Phase::Empty;
        uint64_t             invocation_id = 0;
        std::vector<double>  values;
        std::vector<uint8_t> arrived;
        uint32_t             arrival_count = 0;
        double               reduced_value = 0.0;
        size_t               next_reduction_branch = 0;
        uint32_t             latency_remaining = 0;
        std::vector<uint8_t>  result_sent;
        size_t               result_count = 0;
        size_t               fanout_cursor = 0;
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

    explicit Impl(uint32_t ops_per_cycle = 1, uint32_t latency_cycles = 1) :
        reduction_ops_per_cycle(ops_per_cycle),
        reduction_latency_cycles(latency_cycles)
    {}

    bool outputsSupported(const SST::Merlin::NetworkServiceHost* host,
        const MerlinStaticCollectiveRouteProjection& candidate_projection) const
    {
        if ( host == nullptr ) return false;
        const auto output_supported = [host](int route_vn, uint32_t port) {
            return host->supportsNetworkServiceOutput({ route_vn, static_cast<int>(port),
                static_cast<size_t>(STATIC_COLLECTIVE_REQUEST_BITS) });
        };
        if ( !candidate_projection.root && !output_supported(reduce_vn, *candidate_projection.parent_port) ) {
            return false;
        }
        for ( const auto& branch : candidate_projection.child_branches ) {
            if ( !output_supported(result_vn, branch.port) ) return false;
        }
        for ( const auto& branch : candidate_projection.local_endpoint_branches ) {
            if ( !output_supported(result_vn, branch.port) ) return false;
        }
        return true;
    }

    bool transportSupported(const SST::Merlin::NetworkServiceHost* host) const
    {
        return installed && outputsSupported(host, projection);
    }

    bool install(const SST::Merlin::NetworkServiceHost* host, RouteIdV1 offered_route,
        MerlinStaticCollectiveRouteProjection&& offered_projection, int offered_reduce_vn,
        int offered_result_vn)
    {
        if ( host == nullptr || reduction_ops_per_cycle == 0 ||
             !offered_route.valid() ||
             !offered_projection.valid() || offered_reduce_vn < 0 || offered_result_vn < 0 ||
             offered_reduce_vn == offered_result_vn ) {
            return false;
        }

        const size_t branch_count = offered_projection.child_branches.size() +
                                    offered_projection.local_endpoint_branches.size();
        if ( branch_count == 0 || branch_count > UINT32_MAX ) {
            return false;
        }

        reduce_vn = offered_reduce_vn;
        result_vn = offered_result_vn;
        if ( !outputsSupported(host, offered_projection) ) {
            return false;
        }

        route      = offered_route;
        projection = std::move(offered_projection);
        rebuildBranches();
        installed = true;

        add(statistics.installed_branch_slots, branch_count);
        return true;
    }

    void rebuildBranches()
    {
        branches.clear();
        branches.reserve(projection.child_branches.size() + projection.local_endpoint_branches.size());
        for ( const auto& branch : projection.child_branches ) {
            branches.push_back({ branch.port, branch.representative, false });
        }
        for ( const auto& branch : projection.local_endpoint_branches ) {
            branches.push_back({ branch.port, branch.representative, true });
        }
        branch_by_port.clear();
        branch_by_port.reserve(branches.size());
        for ( size_t index = 0; index < branches.size(); ++index ) {
            branch_by_port.emplace(branches[index].port, index);
        }
        active.values.assign(branches.size(), 0.0);
        active.arrived.assign(branches.size(), 0);
        active.result_sent.assign(branches.size(), 0);
    }

    SST::Merlin::NetworkServiceDecision inspect(int input_port, int input_vn,
        const SST::Merlin::internal_router_event* event) const;
    void consume(int input_port, int input_vn,
        const SST::Merlin::internal_router_event& event) noexcept;

    bool hasScheduledWork() const
    {
        return active.phase == Phase::Reducing || active.phase == Phase::Upward ||
               active.phase == Phase::FanoutResult;
    }

    bool progress(SST::Merlin::NetworkServiceHost* host)
    {
        if ( active.phase == Phase::Reducing ) {
            // A router tick either performs a bounded amount of ordered
            // arithmetic or advances the configured result latency.
            if ( active.next_reduction_branch < branches.size() ) {
                for ( uint32_t count = 0; count < reduction_ops_per_cycle &&
                        active.next_reduction_branch < branches.size(); ++count ) {
                    volatile double rounded = active.reduced_value +
                        active.values[active.next_reduction_branch++];
                    active.reduced_value = rounded;
                }
                if ( active.next_reduction_branch < branches.size() || active.latency_remaining != 0 ) {
                    return false;
                }
            }
            else if ( active.latency_remaining != 0 && --active.latency_remaining != 0 ) {
                return false;
            }
            active.phase = projection.root ? Phase::FanoutResult : Phase::Upward;
        }

        if ( host == nullptr ) return !hasScheduledWork();
        if ( active.phase == Phase::Upward ) {
            if ( !emit(host, CollectiveDirection::Contribution, projection.subtree_representative,
                    projection.root_representative, *projection.parent_port) ) return false;
            active.phase = Phase::AwaitingResult;
        }
        else if ( active.phase == Phase::FanoutResult ) {
            // Keep the scalar and unsent branch bits, not a packet per branch.
            // A blocked destination must not hide independent ready outputs.
            const size_t start = active.fanout_cursor;
            for ( size_t count = 0; count < branches.size(); ++count ) {
                const size_t index = (start + count) % branches.size();
                if ( active.result_sent[index] != 0 ) continue;
                const Branch& branch = branches[index];
                if ( emit(host, CollectiveDirection::Result, projection.root_representative,
                        branch.representative, branch.port) ) {
                    active.result_sent[index] = 1;
                    ++active.result_count;
                    active.fanout_cursor = (index + 1) % branches.size();
                }
            }
            if ( active.result_count != branches.size() ) return false;
            retired_invocation_id = active.invocation_id;
            has_retired_invocation = true;
            clearActive();
        }
        return !hasScheduledWork();
    }

    bool quiescent() const { return active.phase == Phase::Empty; }

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
        active.reduced_value = 0.0;
        active.next_reduction_branch = 0;
        active.latency_remaining = 0;
        active.result_count = 0;
        active.fanout_cursor = 0;
        std::fill(active.result_sent.begin(), active.result_sent.end(), uint8_t { 0 });
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

    void serialize_order(SST::Core::Serialization::serializer& ser)
    {
        SST_SER(installed);
        route.serialize_order(ser);
        projection.serialize_order(ser);
        SST_SER(reduce_vn);
        SST_SER(result_vn);
        SST_SER(retired_invocation_id);
        SST_SER(has_retired_invocation);
        SST_SER(reduction_ops_per_cycle);
        SST_SER(reduction_latency_cycles);
        SST_SER(active_high_water_reported);
        SST_SER(statistics.local_contributions);
        SST_SER(statistics.child_contributions);
        SST_SER(statistics.parent_results);
        SST_SER(statistics.upward_aggregates);
        SST_SER(statistics.result_packets);
        SST_SER(statistics.active_high_water);
        SST_SER(statistics.installed_branch_slots);
        SST_SER(statistics.egress_retries);
        if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
            // Only quiescent processors are checkpointable; runtime scratch
            // and lookup tables are derived from the restored projection.
            clearActive();
            rebuildBranches();
        }
    }

    bool installed = false;
    RouteIdV1 route;
    MerlinStaticCollectiveRouteProjection projection;
    int reduce_vn = 0;
    int result_vn = 1;
    std::vector<Branch> branches;
    std::unordered_map<uint32_t, size_t> branch_by_port;
    ActiveState active;
    uint64_t retired_invocation_id = 0;
    bool has_retired_invocation = false;
    uint32_t reduction_ops_per_cycle = 1;
    uint32_t reduction_latency_cycles = 1;
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

    SST::Merlin::NetworkServiceDecision inspect(int input_port, int input_vn,
        const SST::Merlin::internal_router_event* event, IngressFacts* facts) const;

    bool emit(SST::Merlin::NetworkServiceHost* host, CollectiveDirection direction,
        const MerlinStaticCollectiveRepresentative& source,
        const MerlinStaticCollectiveRepresentative& destination, uint32_t output_port)
    {
        const int vn = direction == CollectiveDirection::Result ? result_vn : reduce_vn;
        if ( !host->canEnqueueNetworkServiceOutput(
                { vn, static_cast<int>(output_port), static_cast<size_t>(STATIC_COLLECTIVE_REQUEST_BITS) }) ) {
            add(statistics.egress_retries, 1);
            return false;
        }
        auto packet = makeEgress(active.invocation_id, active.reduced_value, direction,
            source, destination, output_port);
        if ( !host->tryEnqueueNetworkServiceOutput(CollectiveServiceData::SERVICE_ID, packet) ) {
            add(statistics.egress_retries, 1);
            return false;
        }
        add(direction == CollectiveDirection::Result ? statistics.result_packets : statistics.upward_aggregates, 1);
        return true;
    }

    SST::Merlin::NetworkServiceSyntheticPacket makeEgress(uint64_t invocation_id, double value,
        CollectiveDirection direction, const MerlinStaticCollectiveRepresentative& source,
        const MerlinStaticCollectiveRepresentative& destination, uint32_t output_port) const
    {
        const bool result = direction == CollectiveDirection::Result;
        const int vn = result ? result_vn : reduce_vn;
        auto request = std::make_unique<Request>(destination.physical_endpoint_id,
            source.caller_visible_logical_nid,
            static_cast<size_t>(STATIC_COLLECTIVE_REQUEST_BITS), true, true);
        request->vn             = vn;
        request->allow_adaptive = false;
        auto data = std::make_unique<CollectiveServiceData>(
            route, invocation_id, direction, STATIC_COLLECTIVE_SIGNATURE_V1, 0, encodeValue(value));
        request->giveServiceData(data.get());
        data.release();

        SST::Merlin::NetworkServiceSyntheticPacket packet;
        packet.request     = std::move(request);
        packet.trusted_src = source.physical_endpoint_id;
        packet.route_vn    = vn;
        packet.output_port = static_cast<int>(output_port);
        return packet;
    }

    friend class MerlinStaticCollectiveProcessor;
};

SST::Merlin::NetworkServiceDecision
MerlinStaticCollectiveProcessor::Impl::inspect(
    int input_port, int input_vn, const SST::Merlin::internal_router_event* event) const
{
    return inspect(input_port, input_vn, event, nullptr);
}

SST::Merlin::NetworkServiceDecision
MerlinStaticCollectiveProcessor::Impl::inspect(int input_port, int input_vn,
    const SST::Merlin::internal_router_event* event, IngressFacts* facts) const
{
    using SST::Merlin::NetworkServiceDisposition;

    if ( !installed || event == nullptr || input_port < 0 || input_vn < 0 ) {
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
    int expected_vn = -1;

    if ( contribution ) {
        branch_index = findBranch(input_port);
        if ( branch_index == branches.size() ) return reject(DIAGNOSTIC_WRONG_PORT);
        expected_source      = &branches[branch_index].representative;
        expected_destination = projection.root_representative.physical_endpoint_id;
        expected_vn          = reduce_vn;
    }
    else if ( result ) {
        if ( projection.root || !projection.parent_port ||
             input_port != static_cast<int>(*projection.parent_port) ) {
            return reject(DIAGNOSTIC_WRONG_PORT);
        }
        expected_source       = &projection.root_representative;
        expected_destination  = projection.subtree_representative.physical_endpoint_id;
        expected_vn           = result_vn;
    }
    else {
        return reject(DIAGNOSTIC_UNSUPPORTED);
    }

    const CollectiveDirection expected_direction = contribution ?
        CollectiveDirection::Contribution : CollectiveDirection::Result;
    if ( !data->validFor(route, expected_direction, request->size_in_bits) ) {
        return reject(DIAGNOSTIC_MALFORMED_PACKET);
    }
    if ( data->signature != STATIC_COLLECTIVE_SIGNATURE_V1 || data->chunk_index != 0 ) {
        return reject(DIAGNOSTIC_UNSUPPORTED);
    }
    if ( input_vn != expected_vn || request->vn != expected_vn ) {
        return reject(DIAGNOSTIC_WRONG_VN);
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
    }
    else {
        if ( active.phase == Phase::Reducing || active.phase == Phase::Upward ) {
            return { NetworkServiceDisposition::Busy, DIAGNOSTIC_BUSY_REDUCTION };
        }
        if ( active.phase != Phase::AwaitingResult ) {
            return reject(DIAGNOSTIC_UNEXPECTED);
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
    int input_port, int input_vn, const SST::Merlin::internal_router_event& event) noexcept
{
    IngressFacts facts;
    if ( inspect(input_port, input_vn, &event, &facts).disposition !=
         SST::Merlin::NetworkServiceDisposition::Accept ) {
        std::terminate();
    }

    if ( facts.contribution ) {
        const bool starts_active = active.phase == Phase::Empty;
        const bool completes_reduction = active.arrival_count + 1 == branches.size();
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
            active.phase = Phase::Reducing;
            active.reduced_value = active.values.front();
            active.next_reduction_branch = 1;
            active.latency_remaining = reduction_latency_cycles;
        }
    }
    else {
        active.phase = Phase::FanoutResult;
        active.reduced_value = facts.value;
        add(statistics.parent_results, 1);
    }
}

MerlinStaticCollectiveProcessor::MerlinStaticCollectiveProcessor(
    SST::ComponentId_t id, SST::Params& params, SST::Merlin::NetworkServiceHost* host) :
    SST::Merlin::NetworkServiceProcessor(id, host),
    impl_(std::make_unique<Impl>(params.find<uint32_t>("reduction_ops_per_cycle", 1), params.find<uint32_t>("reduction_latency_cycles", 1)))
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
    const int reduce_vn = params.find<int>("reduce_vn", 0);
    const int result_vn = params.find<int>("result_vn", 1);
    MerlinStaticCollectiveRouteProjection projection;
    if ( !makeStaticProjection(params, projection) ||
         !impl_->install(this->host(), route, std::move(projection), reduce_vn, result_vn) ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "merlin.collective_static_processor received an invalid or unsupported static local projection\n");
    }
}

MerlinStaticCollectiveProcessor::MerlinStaticCollectiveProcessor(SST::Merlin::NetworkServiceHost* host,
    RouteIdV1 route, MerlinStaticCollectiveRouteProjection local_projection,
    int reduce_vn, int result_vn,
    uint32_t reduction_ops_per_cycle, uint32_t reduction_latency_cycles) :
    SST::Merlin::NetworkServiceProcessor(),
    impl_(std::make_unique<Impl>(reduction_ops_per_cycle, reduction_latency_cycles))
{
    bindHost(host);
    if ( !impl_->install(host, route, std::move(local_projection), reduce_vn, result_vn) ) {
        throw std::invalid_argument("Invalid static Merlin collective test projection");
    }
}

MerlinStaticCollectiveProcessor::MerlinStaticCollectiveProcessor() :
    SST::Merlin::NetworkServiceProcessor(),
    impl_(std::make_unique<Impl>(0))
{}

MerlinStaticCollectiveProcessor::~MerlinStaticCollectiveProcessor() = default;

std::vector<int>
MerlinStaticCollectiveProcessor::ownedVNs() const
{
    return { impl_->reduce_vn, impl_->result_vn };
}

SST::Merlin::NetworkServiceDecision
MerlinStaticCollectiveProcessor::inspect(const SST::Merlin::NetworkServiceIngress& ingress) const
{
    return impl_->inspect(ingress.input_port, ingress.input_vn, ingress.event);
}

void
MerlinStaticCollectiveProcessor::consume(
    SST::Merlin::NetworkServiceOwnedIngress ingress) noexcept
{
    auto event = std::move(ingress.event);
    if ( event == nullptr ) std::terminate();
    impl_->consume(ingress.input_port, ingress.input_vn, *event);
    if ( impl_->hasScheduledWork() && host() != nullptr ) host()->wakeNetworkServiceProcessor();
}

bool
MerlinStaticCollectiveProcessor::hasScheduledWork() const
{
    return impl_ != nullptr && impl_->hasScheduledWork();
}

bool
MerlinStaticCollectiveProcessor::progress()
{
    return impl_->progress(host());
}

bool
MerlinStaticCollectiveProcessor::validateInstalledTransport() const
{
    return impl_ != nullptr && impl_->transportSupported(host());
}

void
MerlinStaticCollectiveProcessor::serialize_order(SST::Core::Serialization::serializer& ser)
{
    SST::Merlin::NetworkServiceProcessor::serialize_order(ser);
    if ( ser.mode() != SST::Core::Serialization::serializer::UNPACK && !impl_->quiescent() ) {
        getSimulationOutput().fatal(CALL_INFO, 1,
            "merlin.collective_static_processor cannot checkpoint with an active invocation or pending egress\n");
    }
    impl_->serialize_order(ser);
}

} // namespace SST::Collective
