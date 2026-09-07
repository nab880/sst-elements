// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.
#include <sst_config.h>
#include "hr_router/hr_router.h"

#include <sst/core/params.h>
#include <sst/core/output.h>
#include <sst/core/timeLord.h>
#include <sst/core/unitAlgebra.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>

#include <signal.h>

#include "merlin.h"
#include "interfaces/portControl.h"

using namespace SST::Merlin;
using namespace SST::Interfaces;
using namespace std;

NetworkServiceSyntheticRequester::NetworkServiceSyntheticRequester(int num_vcs, uint32_t capacity) :
    num_vcs_(num_vcs), capacity_(capacity), queues_(num_vcs),
    heads_(num_vcs, nullptr), next_output_(num_vcs, 0)
{}

NetworkServiceSyntheticRequester::~NetworkServiceSyntheticRequester()
{
    for ( auto& by_output : queues_ ) {
        for ( auto& entry : by_output ) {
            while ( !entry.second.empty() ) {
                delete entry.second.front();
                entry.second.pop();
            }
        }
    }
}

bool
NetworkServiceSyntheticRequester::canEnqueue(int vc) const
{
    return vc >= 0 && vc < num_vcs_ && capacity_ != 0 && size_ < capacity_;
}

bool
NetworkServiceSyntheticRequester::enqueue(std::unique_ptr<internal_router_event>& event, int vc)
{
    if ( !event || event->getNextPort() < 0 || !canEnqueue(vc) ) return false;
    auto& queue = queues_[vc][event->getNextPort()];
    queue.push(event.release());
    if ( heads_[vc] == nullptr ) heads_[vc] = queue.front();
    ++size_;
    return true;
}

void
NetworkServiceSyntheticRequester::selectReadyHeads(PortInterface** outputs, const int* output_busy)
{
    for ( int vc = 0; vc < num_vcs_; ++vc ) {
        auto& by_output = queues_[vc];
        heads_[vc] = nullptr;
        if ( by_output.empty() ) continue;
        auto current = by_output.lower_bound(next_output_[vc]);
        if ( current == by_output.end() ) current = by_output.begin();
        for ( size_t checked = 0; checked < by_output.size(); ++checked ) {
            auto* event = current->second.front();
            // Retain a blocked head for stall accounting if every output is blocked.
            if ( heads_[vc] == nullptr ) heads_[vc] = event;
            if ( output_busy[current->first] == 0 &&
                 outputs[current->first]->spaceToSend(event->getVC(), event->getFlitCount()) ) {
                heads_[vc] = event;
                break;
            }
            if ( ++current == by_output.end() ) current = by_output.begin();
        }
    }
}

internal_router_event*
NetworkServiceSyntheticRequester::recv(int vc)
{
    if ( vc < 0 || vc >= num_vcs_ || heads_[vc] == nullptr ) return nullptr;
    internal_router_event* event = heads_[vc];
    auto& by_output = queues_[vc];
    auto current = by_output.find(event->getNextPort());
    if ( current == by_output.end() || current->second.front() != event ) {
        throw std::logic_error("Synthetic requester selected a non-head packet");
    }
    next_output_[vc] = current->first + 1;
    current->second.pop();
    if ( current->second.empty() ) by_output.erase(current);
    heads_[vc] = by_output.empty() ? nullptr : by_output.begin()->second.front();
    --size_;
    return event;
}

void
NetworkServiceSyntheticRequester::serialize_order(SST::Core::Serialization::serializer& ser)
{
    SST_SER(num_vcs_);
    SST_SER(capacity_);
    SST_SER(size_);
    SST_SER(queues_);
    SST_SER(next_output_);
    if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
        if ( num_vcs_ <= 0 || queues_.size() != static_cast<size_t>(num_vcs_) ||
             next_output_.size() != queues_.size() ) {
            throw std::runtime_error("Invalid serialized Merlin network-service queue dimensions");
        }
        heads_.assign(num_vcs_, nullptr);
        uint64_t actual_size = 0;
        for ( int vc = 0; vc < num_vcs_; ++vc ) {
            for ( const auto& entry : queues_[vc] ) {
                if ( entry.first < 0 || entry.second.empty() ||
                     entry.second.front()->getNextPort() != entry.first ) {
                    throw std::runtime_error("Invalid serialized Merlin network-service output queue");
                }
                actual_size += entry.second.size();
            }
            if ( !queues_[vc].empty() ) heads_[vc] = queues_[vc].begin()->second.front();
        }
        if ( actual_size != size_ || size_ > capacity_ ) {
            throw std::runtime_error("Invalid serialized Merlin network-service synthetic queue size");
        }
    }
}

// Helper functions used only in this file
static string trim(string str)
{
    // Find whitespace in front
    int front_index = 0;
    while ( isspace(str[front_index]) ) front_index++;

    // Find whitespace in back
    int back_index = str.length() - 1;
    while ( isspace(str[back_index]) ) back_index--;

    return str.substr(front_index,back_index-front_index+1);
}

static void split(string input, string delims, vector<string>& tokens) {
    if ( input.length() == 0 ) return;
    size_t start = 0;
    size_t stop = 0;;
    vector<string> ret;

    do {
        stop = input.find_first_of(delims,start);
        tokens.push_back(input.substr(start,stop-start));
        start = stop + 1;
    } while (stop != string::npos);

    for ( unsigned int i = 0; i < tokens.size(); i++ ) {
        tokens[i] = trim(tokens[i]);
    }
}

static std::string getLogicalGroupParam(const Params& params, Topology* topo, int port,
                                        std::string param, std::string default_val = "") {
    // Use topology object to get the group for the port
    std::string group = topo->getPortLogicalGroup(port);

    // Create fully qualified key name
    std::string key = param;
    key.append(std::string(":")).append(group);

    std::string value = params.find<std::string>(key);

    if ( value == "" ) {
        // Look for default value
        value = params.find<std::string>(param, default_val);
        if ( value == "" ) {
            // Abort
            merlin_abort.fatal(CALL_INFO, -1, "hr_router requires %s to be specified\n", param.c_str());
        }
    }
    return value;
}

static UnitAlgebra getLogicalGroupParamUA(const Params& params, Topology* topo, int port,
                                          std::string param, std::string default_val = "") {

    std::string value = getLogicalGroupParam(params,topo,port,param,default_val);

    UnitAlgebra ua(value);
    // If units were in Bytes, convert to bits
    if ( ua.hasUnits("B") || ua.hasUnits("B/s") ) {
        ua *= UnitAlgebra("8b/B");
    }

    return ua;
}


hr_router::hr_router() :
    Router(),
    id(0),
    num_ports(0),
    num_vns(0),
    vn_remap_shm_size(0),
    num_vcs(0),
    flit_size_bits(0),
    topo(nullptr),
    arb(nullptr),
    ports(nullptr),
    vc_heads(nullptr),
    xbar_in_credits(nullptr),
    output_queue_lengths(nullptr),
#if VERIFY_DECLOCKING
    clocking(false),
#endif
    in_port_busy(nullptr),
    out_port_busy(nullptr),
    progress_vcs(nullptr),
    unclocked_cycle(0),
    my_clock_handler(nullptr),
    xbar_stalls(nullptr),
    output(getSimulationOutput())
{}

void hr_router::serialize_order(SST::Core::Serialization::serializer& ser) {
    Router::serialize_order(ser);

    SST_SER(id);
    SST_SER(num_ports);
    SST_SER(num_vns);
    SST_SER(vn_remap_shm);
    SST_SER(vn_remap_shm_size);
    SST_SER(num_vcs);
    SST_SER(flit_size_bits);
    SST_SER(vcs_per_vn);

    SST_SER(topo);
    SST_SER(arb);

    bool has_network_service = network_service != nullptr;
    SST_SER(has_network_service);
    if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
        if ( has_network_service ) network_service.reset(new NetworkServiceRouterContext());
        else network_service.reset();
    }
    if ( has_network_service ) {
        if ( ser.mode() != SST::Core::Serialization::serializer::UNPACK &&
             network_service->hasIngressWork() ) {
            output.fatal(CALL_INFO, 1, "Merlin router cannot checkpoint with a staged network-service ingress transfer\n");
        }
        SST_SER(network_service->ingress_width);
        SST_SER(network_service->ingress_flits_per_cycle);
        SST_SER(network_service->shared_ingress);
        SST_SER(network_service->ingress_busy);
        SST_SER(network_service->ingress_vcs);
        SST_SER(network_service->processor);
        SST_SER(network_service->output_queue_depth);
        SST_SER(network_service->scan_cursor);

        if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
            network_service->requester.reset(new NetworkServiceSyntheticRequester(
                num_vcs, network_service->output_queue_depth));
        }
        network_service->requester->serialize_order(ser);

        SST_SER(network_service->accept);
        SST_SER(network_service->busy);
        SST_SER(network_service->reject);
        SST_SER(network_service->synthetic);
        SST_SER(network_service->synthetic_stall);
    }

    size_t total_vcs = num_ports * num_vcs;
    SST_SER(SST::Core::Serialization::array(xbar_in_credits, total_vcs));
    SST_SER(SST::Core::Serialization::array(output_queue_lengths, total_vcs));

    SST_SER(SST::Core::Serialization::array(ports, num_ports));

    // vc_heads has duplicate data from the PortControls.  The actual data is checkpointed in the PortControl objects
    // and is restored below, just need to recreate the array.
    if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
        vc_heads = new internal_router_event*[num_ports * num_vcs];
        for ( int i = 0; i < num_ports * num_vcs; i++ ) {
            vc_heads[i] = nullptr;
        }
    }

#if VERIFY_DECLOCKING
    SST_SER(clocking);
#endif

    int num_xbar_inputs = numXbarInputs();
    SST_SER(SST::Core::Serialization::array(in_port_busy, num_xbar_inputs));
    SST_SER(SST::Core::Serialization::array(out_port_busy, num_ports));
    SST_SER(SST::Core::Serialization::array(progress_vcs, num_xbar_inputs));

    SST_SER(input_buf_size);
    SST_SER(output_buf_size);
    SST_SER(unclocked_cycle);
    SST_SER(xbar_bw);
    SST_SER(xbar_tc);
    SST_SER(my_clock_handler);
    SST_SER(inspector_names);

    SST_SER(SST::Core::Serialization::array(xbar_stalls, num_ports));

    SST_SER(shared_array);

    if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
        output = getSimulationOutput();

        // Re-establish non-owning pointers in each PortControl.
        // Must NOT call initVCs() here — it unconditionally allocates
        // new buffers and credits, overwriting the deserialized state
        // from PortControl::serialize_order(). Instead, only reconnect
        // the shared array slices (vc_heads, xbar_in_credits,
        // output_queue_lengths) and the topo/parent pointers.
        for ( int i = 0; i < num_ports; i++ ) {
            PortControl* pc = dynamic_cast<PortControl*>(ports[i]);
            if ( pc ) {
                pc->restoreSharedArrays(
                    &vc_heads[i * num_vcs],
                    &xbar_in_credits[i * num_vcs],
                    &output_queue_lengths[i * num_vcs]);
                pc->repopulateVCHeads();
            }
        }

        topo->setOutputBufferCreditArray(xbar_in_credits, num_vcs);
        topo->setOutputQueueLengthsArray(output_queue_lengths, num_vcs);

        if ( network_service ) {
            network_service->rebuildInputs(ports, num_ports);
            network_service->processor->bindHost(this);
            resolveOwnedVNs();
        }
    }
}

hr_router::~hr_router()
{
    delete [] in_port_busy;
    delete [] out_port_busy;
    delete [] progress_vcs;

    // SST framework manages SubComponent lifecycle — do not delete ports[i], topo, or arb
    delete [] ports;
}

hr_router::hr_router(ComponentId_t cid, Params& params) :
    Router(cid),
    num_vcs(-1),
    flit_size_bits(0),
    output(getSimulationOutput())
{

    // Get the options for the router
    id = params.find<int>("id",-1);
    if ( id == -1 ) {
        merlin_abort.fatal(CALL_INFO, -1, "hr_router requires id to be specified\n");
    }

    num_ports = params.find<int>("num_ports",-1);
    if ( num_ports == -1 ) {
        merlin_abort.fatal(CALL_INFO, -1, "hr_router requires num_ports to be specified\n");
    }


    // Get the number of VNs
    num_vns = params.find<int>("num_vns",2);
    vcs_per_vn.resize(num_vns);

    // Get the topology
    topo = (Topology*)loadUserSubComponent<SST::Merlin::Topology>
        ("topology", ComponentInfo::SHARE_NONE, num_ports, id, num_vns);

    if ( !topo ) {
        merlin_abort.fatal(CALL_INFO_LONG, 1, "hr_router requires topology to be specified in input file\n");
    }

    topo->getVCsPerVN(vcs_per_vn);
    num_vcs = 0;
    for ( int vcs : vcs_per_vn ) num_vcs += vcs;

    // Check to see if remap is on
    vn_remap_shm = params.find<std::string>("vn_remap_shm","");
    if ( vn_remap_shm != "" ) {
        // If I'm id 0, create the shared region
        std::vector<int> vec;
        params.find_array<int>("vn_remap",vec);
        if ( vec.size() == 0 ) {
            merlin_abort.fatal(CALL_INFO, 1, "if vn_remap_shm is specified, a map must be supplied using vn_remap\n");
        }
        vn_remap_shm_size = vec.size() * sizeof(int);
        if ( id == 0 ) {
            shared_array.initialize(vn_remap_shm, vn_remap_shm_size);
            for ( int i = 0; i < vec.size(); ++i ) {
                shared_array.write(i,vec[i]);
            }
            shared_array.publish();
        }
    }

    // Parse all the timing parameters

    // Flit size
    std::string flit_size_s = params.find<std::string>("flit_size");
    if ( flit_size_s == "" ) {
        merlin_abort.fatal(CALL_INFO, -1, "hr_router requires flit_size to be specified\n");
        abort();
    }
    UnitAlgebra flit_size(flit_size_s);

    if ( flit_size.hasUnits("B") ) {
        // Need to convert to bits per second
        flit_size *= UnitAlgebra("8b/B");
    }
    flit_size_bits = flit_size.getRoundedValue();
    if ( flit_size_bits <= 0 ) {
        merlin_abort.fatal(CALL_INFO, 1, "hr_router flit_size must round to a positive number of bits\n");
    }

    // Link BW default.  Can be overwritten using logical groups
    std::string link_bw_s = params.find<std::string>("link_bw");
    UnitAlgebra link_bw(link_bw_s);

    if ( link_bw.hasUnits("B/s") ) {
        // Need to convert to bits per second
        link_bw *= UnitAlgebra("8b/B");
    }

    // Cross bar bandwidth
    std::string xbar_bw_s = params.find<std::string>("xbar_bw");
    if ( xbar_bw_s == "" ) {
        merlin_abort.fatal(CALL_INFO, -1, "hr_router requires xbar_bw to be specified\n");
    }

    UnitAlgebra xbar_bw_ua(xbar_bw_s);
    if ( xbar_bw_ua.hasUnits("B/s") ) {
        // Need to convert to bits per second
        xbar_bw_ua *= UnitAlgebra("8b/B");
    }

    UnitAlgebra xbar_clock;
    xbar_clock = xbar_bw_ua / flit_size;


    std::string input_latency = params.find<std::string>("input_latency", "0ns");
    std::string output_latency = params.find<std::string>("output_latency", "0ns");


    // Create all the PortControl blocks
    ports = new PortInterface*[num_ports];

    std::string input_buf_size = params.find<std::string>("input_buf_size", "0");
    std::string output_buf_size = params.find<std::string>("output_buf_size", "0");


    std::string inspector_config = params.find<std::string>("network_inspectors", "");
    split(inspector_config,",",inspector_names);

    bool oql_track_port = params.find<bool>("oql_track_port","false");
    bool oql_track_remote = params.find<bool>("oql_track_remote","false");

    params.enableVerify(false);

    Params pc_params = params.get_scoped_params("portcontrol");

    pc_params.insert("flit_size", flit_size.toStringBestSI());
    if (pc_params.contains("network_inspectors")) pc_params.insert("network_inspectors", params.find<std::string>("network_inspectors", ""));
    pc_params.insert("oql_track_port", params.find<std::string>("oql_track_port","false"));
    pc_params.insert("oql_track_remote", params.find<std::string>("oql_track_remote","false"));

    for ( int i = 0; i < num_ports; i++ ) {
        std::stringstream port_name;
        port_name << "port";
        port_name << i;

        // For each port, some default parameters can be overwritten
        // by logical group parameters (link_bw, input_buf_size,
        // output_buf_size, input_latency, output_latency).

        pc_params.insert("port_name", port_name.str());
        pc_params.insert("link_bw", getLogicalGroupParam(params,topo,i,"link_bw") );
        pc_params.insert("input_latency", getLogicalGroupParam(params,topo,i,"input_latency","0ns"));
        pc_params.insert("output_latency", getLogicalGroupParam(params,topo,i,"output_latency","0ns"));
        pc_params.insert("input_buf_size", getLogicalGroupParam(params,topo,i,"input_buf_size"));
        pc_params.insert("output_buf_size", getLogicalGroupParam(params,topo,i,"output_buf_size"));
        pc_params.insert("dlink_thresh", getLogicalGroupParam(params,topo,i,"dlink_thresh", "-1"));
        pc_params.insert("vn_remap_shm", vn_remap_shm);
        pc_params.insert("vn_remap_shm_size", std::to_string(vn_remap_shm_size));
        pc_params.insert("num_vns", std::to_string(num_vns));

        ports[i] = loadAnonymousSubComponent<PortInterface>
            ("merlin.portcontrol","portcontrol", i, ComponentInfo::SHARE_PORTS | ComponentInfo::SHARE_STATS | ComponentInfo::INSERT_STATS,
             pc_params,this,id,i,topo);
    }
    params.enableVerify(true);

    const uint32_t network_service_output_queue_depth =
        params.find<uint32_t>("network_service_output_queue_depth", 8);
    NetworkServiceProcessor* network_service_processor = loadUserSubComponent<NetworkServiceProcessor>(
        "network_service", ComponentInfo::SHARE_NONE, this);
    if ( network_service_processor ) {
        if ( network_service_processor->getServiceID() == SimpleNetwork::NETWORK_SERVICE_NONE ||
             network_service_output_queue_depth == 0 ||
             !network_service_processor->getRequestContract().valid() ||
             network_service_processor->getRequestContract().service_id !=
                 network_service_processor->getServiceID() ) {
            merlin_abort.fatal(CALL_INFO, 1,
                "Network service processor requires a nonzero service ID and output queue depth\n");
        }
        network_service.reset(new NetworkServiceRouterContext());
        network_service->processor = network_service_processor;
        network_service->output_queue_depth = network_service_output_queue_depth;
        network_service->ingress_width = params.find<uint32_t>("network_service_ingress_width", 1);
        network_service->ingress_flits_per_cycle =
            params.find<uint32_t>("network_service_ingress_flits_per_cycle", 1);
        network_service->shared_ingress = params.find<bool>("network_service_shared_ingress", true);
        if ( network_service->ingress_width == 0 || network_service->ingress_flits_per_cycle == 0 ) {
            merlin_abort.fatal(CALL_INFO, 1, "Network service ingress width and bandwidth must be positive\n");
        }
        network_service->ingress_busy.assign(static_cast<size_t>(num_ports), 0);
        network_service->ingress_vcs.assign(static_cast<size_t>(num_ports), -1);
        network_service->requester.reset(
            new NetworkServiceSyntheticRequester(num_vcs, network_service_output_queue_depth));
        network_service->rebuildInputs(ports, num_ports);
        resolveOwnedVNs();
    }

    std::string xbar_arb = params.find<std::string>("xbar_arb", "merlin.xbar_arb_lru");
    Params empty_params;
    arb = loadAnonymousSubComponent<XbarArbitration>(
        xbar_arb, "XbarArb", 0, ComponentInfo::INSERT_STATS, empty_params);

    // Service-only input state is absent when no processor is installed.
    in_port_busy = new int[numXbarInputs()];
    out_port_busy = new int[num_ports];
    progress_vcs = new int[numXbarInputs()];
    for ( int i = 0; i < num_ports; ++i ) {
        in_port_busy[i] = 0;
        out_port_busy[i] = 0;
        progress_vcs[i] = -1;
    }
    if ( network_service ) {
        in_port_busy[num_ports] = 0;
        progress_vcs[num_ports] = -1;
    }

    my_clock_handler = new Clock::Handler<hr_router,&hr_router::clock_handler>(this);
    xbar_tc = registerClock( xbar_clock, my_clock_handler);

#if VERIFY_DECLOCKING
    clocking = true;
#endif

    // Check to make sure that the xbar BW is equal to or greater than
    // the link BW, otherwise the model runs into problems
    // if ( xbar_tc->getFactor() > link_tc->getFactor() ) {
    if ( xbar_bw_ua < link_bw  ) {
        merlin_abort.fatal(CALL_INFO_LONG,1,"ERROR: hr_router requires xbar_bw to be greater than or equal to link_bw\n"
              "  xbar_bw = %s, link_bw = %s\n",
              xbar_bw_ua.toStringBestSI().c_str(), link_bw.toStringBestSI().c_str());
    }
    // Register statistics
    xbar_stalls = new Statistic<uint64_t>*[num_ports];
    for ( int i = 0; i < num_ports; i++ ) {
        std::string port_name("port");
        port_name = port_name + std::to_string(i);
        xbar_stalls[i] = registerStatistic<uint64_t>("xbar_stalls",port_name);
    }
    if ( network_service ) {
        network_service->accept = registerStatistic<uint64_t>("network_service_accept");
        network_service->busy = registerStatistic<uint64_t>("network_service_busy");
        network_service->reject = registerStatistic<uint64_t>("network_service_reject");
        network_service->synthetic = registerStatistic<uint64_t>("network_service_synthetic");
        network_service->synthetic_stall = registerStatistic<uint64_t>("network_service_synthetic_stall");
    }

    init_vcs();
}


void
hr_router::notifyEvent()
{
    setRequestNotifyOnEvent(false);

#if VERIFY_DECLOCKING
    clocking = true;
    Cycle_t next_cycle = getNextClockCycle( xbar_tc );
#else
    Cycle_t next_cycle = reregisterClock( xbar_tc, my_clock_handler);
#endif

    int64_t elapsed_cycles = next_cycle - unclocked_cycle;


#if !VERIFY_DECLOCKING
    // Fix up the busy variables
    for ( int i = 0; i < num_ports; i++ ) {
    	// Should stop at zero, need to find a clean way to do this
    	// with no branch.  For now it should work.
        int64_t tmp = in_port_busy[i] - elapsed_cycles;
    	if ( tmp < 0 ) in_port_busy[i] = 0;
        else in_port_busy[i] = tmp;
        tmp = out_port_busy[i] - elapsed_cycles;
    	if ( tmp < 0 ) out_port_busy[i] = 0;
        else out_port_busy[i] = tmp;
    }
    if ( network_service ) {
        const int64_t tmp = in_port_busy[num_ports] - elapsed_cycles;
        in_port_busy[num_ports] = tmp < 0 ? 0 : static_cast<int>(tmp);
    }
#endif
    // Report skipped cycles to arbitration unit.
    arb->reportSkippedCycles(elapsed_cycles);
}

NetworkServiceID
hr_router::getNetworkServiceID() const
{
    return network_service == nullptr ? SimpleNetwork::NETWORK_SERVICE_NONE :
                                        network_service->processor->getServiceID();
}

NetworkServiceRequestContract
hr_router::getNetworkServiceRequestContract() const
{
    return network_service == nullptr ? NetworkServiceRequestContract{} :
                                        network_service->processor->getRequestContract();
}

int
hr_router::firstVCForVN(int vn) const
{
    int first_vc = 0;
    for ( int index = 0; index < vn; ++index ) first_vc += vcs_per_vn[static_cast<size_t>(index)];
    return first_vc;
}

int
hr_router::networkServiceFlits(size_t bits) const
{
    if ( bits == 0 || flit_size_bits <= 0 ) return 0;
    const size_t flit_bits = static_cast<size_t>(flit_size_bits);
    const size_t flits = bits / flit_bits + (bits % flit_bits != 0);
    return flits <= static_cast<size_t>(std::numeric_limits<int>::max()) ? static_cast<int>(flits) : 0;
}

bool
hr_router::supportsNetworkServiceOutput(const NetworkServiceOutputSpec& spec) const
{
    const int flits = networkServiceFlits(spec.size_in_bits);
    if ( !spec.valid() || spec.route_vn >= num_vns || spec.output_port >= num_ports || flits == 0 ||
         ports == nullptr || ports[spec.output_port] == nullptr ||
         !ports[spec.output_port]->isConnected() ||
         flits > ports[spec.output_port]->getFixedOutputCapacityInFlits() ||
         (topo->getPortState(spec.output_port) != Topology::R2N &&
             topo->getPortState(spec.output_port) != Topology::R2R) ) return false;
    const int downstream_capacity =
        ports[spec.output_port]->getFixedDownstreamCapacityInFlits(firstVCForVN(spec.route_vn));
    return downstream_capacity < 0 || flits <= downstream_capacity;
}

bool
hr_router::canEnqueueNetworkServiceOutput(const NetworkServiceOutputSpec& spec) const
{
    return network_service && supportsNetworkServiceOutput(spec) &&
           network_service->requester->canEnqueue(firstVCForVN(spec.route_vn)) &&
           out_port_busy[spec.output_port] == 0 &&
           ports[spec.output_port]->spaceToSend(firstVCForVN(spec.route_vn), networkServiceFlits(spec.size_in_bits));
}

bool
hr_router::tryEnqueueNetworkServiceOutput(
    NetworkServiceID service_id, NetworkServiceSyntheticPacket& packet)
{
    const NetworkServiceOutputSpec spec { packet.route_vn, packet.output_port,
        packet.request ? packet.request->size_in_bits : 0 };
    if ( network_service == nullptr || network_service->requester == nullptr ||
         service_id != network_service->processor->getServiceID() || !packet.valid(service_id) ||
         !supportsNetworkServiceOutput(spec) ) {
        return false;
    }
    const int output_vc = firstVCForVN(packet.route_vn);
    if ( !network_service->requester->canEnqueue(output_vc) ) return false;

    std::unique_ptr<RtrEvent> envelope(
        new RtrEvent(packet.request.release(), packet.trusted_src, packet.route_vn));
    if ( !envelope->setSyntheticTransportMetadata(networkServiceFlits(spec.size_in_bits), getCurrentSimTimeNano()) ) {
        packet.request.reset(envelope->takeRequest());
        return false;
    }

    std::unique_ptr<internal_router_event> event(topo->process_input(envelope.release()));
    if ( !event || !event->hasValidTransportMetadata() ) {
        output.fatal(CALL_INFO, 1, "Topology failed to create a valid synthetic router envelope\n");
    }
    event->setNextPort(packet.output_port);
    event->setVC(output_vc);
    event->setCreditReturnVC(output_vc);
    if ( !network_service->requester->enqueue(event, output_vc) ) {
        RtrEvent* returned_envelope = event->takeEncapsulatedEvent();
        packet.request.reset(returned_envelope->takeRequest());
        delete returned_envelope;
        return false;
    }

    wakeNetworkServiceProcessor();
    return true;
}

void
hr_router::wakeNetworkServiceProcessor()
{
    if ( getRequestNotifyOnEvent() ) notifyEvent();
}

void
hr_router::dumpState(std::ostream& stream)
{
    stream << "Router id: " << id << std::endl;
    for ( int i = 0; i < num_ports; i++ ) {
	ports[i]->dumpState(stream);
	stream << "  Output_busy: " << out_port_busy[i] << std::endl;
	stream << "  Input_Busy: " <<  in_port_busy[i] << std::endl;
    }

}

void
hr_router::printStatus(Output& out)
{
    out.output("Start Router:  id = %d\n", id);
    for ( int i = 0; i < num_ports; i++ ) {
        ports[i]->printStatus(out, out_port_busy[i], in_port_busy[i]);
    }
    out.output("End Router: id = %d\n", id);
}


// The processor names VNs; the router owns the VN-to-VC mapping, so every
// VC the topology assigns to an owned VN belongs to the processor.
void
hr_router::resolveOwnedVNs()
{
    NetworkServiceRouterContext& service = *network_service;
    service.owned_vc_mask.assign(static_cast<size_t>(num_vcs), 0);
    service.owned_vcs.clear();
    std::vector<uint8_t> owned_vn(static_cast<size_t>(num_vns), 0);
    for ( int vn : service.processor->ownedVNs() ) {
        if ( vn < 0 || vn >= num_vns || owned_vn[static_cast<size_t>(vn)] != 0 ) {
            merlin_abort.fatal(CALL_INFO, 1,
                "Network service processor owns VN %d, which is repeated or outside this router's %d VNs\n",
                vn, num_vns);
        }
        owned_vn[static_cast<size_t>(vn)] = 1;
        const int first_vc = firstVCForVN(vn);
        for ( int vc = first_vc; vc < first_vc + vcs_per_vn[static_cast<size_t>(vn)]; ++vc ) {
            service.owned_vc_mask[static_cast<size_t>(vc)] = 1;
            service.owned_vcs.push_back(vc);
        }
    }
    std::sort(service.owned_vcs.begin(), service.owned_vcs.end());
}

// Transfer service heads through bounded per-port ingress resources.  The
// packet stays in PortControl until transfer completion returns its credits.
void
hr_router::serviceOwnedHeads()
{
    NetworkServiceRouterContext& service = *network_service;
    if ( service.owned_vcs.empty() ) return;

    const int start = static_cast<int>(service.scan_cursor % static_cast<uint64_t>(num_ports));
    const auto accepts = [&](int port, int vc, internal_router_event* head) {
        const NetworkServiceDecision decision = service.processor->inspect({ port, head->getVN(), head });
        if ( !isValid(decision.disposition) ) {
            output.fatal(CALL_INFO, 1, "Merlin router %d received an invalid network-service disposition\n", id);
        }
        if ( decision.disposition == NetworkServiceDisposition::Busy ) {
            if ( service.busy ) service.busy->addData(1);
            return false;
        }
        if ( decision.disposition == NetworkServiceDisposition::Reject ) {
            if ( service.reject ) service.reject->addData(1);
            const SimpleNetwork::Request* request = head->inspectRequest();
            output.fatal(CALL_INFO, 1,
                "Merlin router %d rejected a packet on service VC %d from port %d (service %u, opaque "
                "diagnostic 0x%016" PRIx64 ")\n",
                id, vc, port, request == nullptr ? 0u : static_cast<unsigned>(request->getServiceID()),
                decision.opaque_diagnostic);
        }
        return true;
    };

    // Tick only transfers launched in earlier cycles.  The head stays in its
    // physical input queue, retaining its credits and ownership until commit.
    for ( int port = 0; port < num_ports; ++port ) {
        if ( service.ingress_vcs[port] >= 0 && service.ingress_busy[port] != 0 ) {
            --service.ingress_busy[port];
        }
    }

    uint32_t committed = 0;
    for ( int count = 0; count < num_ports && committed < service.ingress_width; ++count ) {
        const int port = (start + count) % num_ports;
        const int vc = service.ingress_vcs[port];
        if ( vc < 0 || service.ingress_busy[port] != 0 ) continue;
        internal_router_event* head = ports[port]->getVCHeads()[vc];
        if ( head == nullptr ) {
            output.fatal(CALL_INFO, 1, "Merlin router %d lost a staged service head on port %d VC %d\n", id, port, vc);
        }
        // Initial inspect() did not reserve processor state.  Re-check after
        // the transfer because another port may have changed that state.
        // Busy cancels this provisional transfer, leaving the head untouched;
        // another VC on this port can then carry traffic needed to unblock it.
        service.ingress_vcs[port] = -1;
        if ( !accepts(port, vc, head) ) continue;

        const int vn = head->getVN();
        internal_router_event* dequeued = ports[port]->recv(vc);
        if ( dequeued != head ) {
            output.fatal(CALL_INFO, 1, "Merlin router %d dequeued a different head than it inspected\n", id);
        }
        if ( service.accept ) service.accept->addData(1);
        NetworkServiceOwnedIngress owned;
        owned.input_port = port;
        owned.input_vn = vn;
        owned.event.reset(dequeued);
        service.processor->consume(std::move(owned));
        ++committed;
    }

    uint32_t started = 0;
    for ( int count = 0; count < num_ports && started < service.ingress_width; ++count ) {
        const int port = (start + count) % num_ports;
        if ( service.ingress_vcs[port] >= 0 ||
             (service.shared_ingress && in_port_busy[port] != 0) ) continue;
        internal_router_event** heads = ports[port]->getVCHeads();
        for ( int vc : service.owned_vcs ) {
            internal_router_event* head = heads[vc];
            if ( head == nullptr || !accepts(port, vc, head) ) continue;
            const int flits = head->getFlitCount();
            if ( flits <= 0 ) {
                output.fatal(CALL_INFO, 1, "Merlin router %d received a service packet with invalid flit count\n", id);
            }
            const uint32_t cycles = 1 + (static_cast<uint32_t>(flits) - 1) / service.ingress_flits_per_cycle;
            service.ingress_vcs[port] = vc;
            service.ingress_busy[port] = cycles;
            // The normal end-of-cycle decrement accounts for this first
            // transfer cycle.  Private service reads use their own resource.
            if ( service.shared_ingress ) in_port_busy[port] = static_cast<int>(cycles);
            ++started;
            break;
        }
    }
    service.scan_cursor = static_cast<uint64_t>(start) + 1;
}

bool
hr_router::clock_handler(Cycle_t cycle)
{
    NetworkServiceRouterContext* const service = network_service.get();
    const bool service_work =
        service != nullptr && (service->hasIngressWork() || service->requester->hasWork() || service->processor->hasScheduledWork());

    // If there are no events in the input queues, then we can remove
    // ourselves from the clock queue, as long as the arbitration unit
    // says it's okay.
    if ( get_vcs_with_data() == 0 && !service_work ) {
#if VERIFY_DECLOCKING
        if ( clocking ) {
            if ( arb->isOkayToPauseClock() ) {
                setRequestNotifyOnEvent(true);
                unclocked_cycle = cycle;
                clocking = false;
            }
        }
#else
        if ( arb->isOkayToPauseClock() ) {
            setRequestNotifyOnEvent(true);
            unclocked_cycle = cycle;
            return true;
        }
        else {
            return false;
        }
#endif
    }

    if ( service != nullptr ) {
        // Drain egress admitted in earlier cycles before consuming new
        // heads, so a processor-created packet always takes at least one
        // cycle to reach arbitration.
        if ( service->processor->hasScheduledWork() ) service->processor->progress();
        serviceOwnedHeads();
        service->requester->selectReadyHeads(ports, out_port_busy);
#if VERIFY_DECLOCKING
        const bool arbitrated = arb->arbitrateNetworkService(
            service->xbar_inputs.data(), ports, in_port_busy, out_port_busy, progress_vcs, clocking);
#else
        const bool arbitrated = arb->arbitrateNetworkService(
            service->xbar_inputs.data(), ports, in_port_busy, out_port_busy, progress_vcs);
#endif
        if ( !arbitrated ) output.fatal(CALL_INFO, 1, "Network-service crossbar arbitration failed; active services require merlin.xbar_arb_lru\n");
    }
    else {
#if VERIFY_DECLOCKING
        arb->arbitrate(ports, in_port_busy, out_port_busy, progress_vcs, clocking);
#else
        arb->arbitrate(ports, in_port_busy, out_port_busy, progress_vcs);
#endif
    }

    // Move the events and decrement the busy values.  Input num_ports is
    // the synthetic requester when a service is installed.
    const int num_inputs = numXbarInputs();
    for ( int i = 0; i < num_inputs; i++ ) {
        if ( progress_vcs[i] > -1 ) {
            internal_router_event* ev = i < num_ports ? ports[i]->recv(progress_vcs[i]) :
                                                        service->requester->recv(progress_vcs[i]);
            ports[ev->getNextPort()]->send(ev, ev->getVC());

            if ( i >= num_ports && service->synthetic ) service->synthetic->addData(1);

            if ( ev->getTraceType() == SimpleNetwork::Request::FULL ) {
                output.output("TRACE(%d): %" PRIu64 " ns: Copying event (src = %d, dest = %d) "
                              "over crossbar in router %d (%s) from port %d, VC %d to port"
                              " %d, VC %d.\n",
                              ev->getTraceID(), getCurrentSimTimeNano(), ev->getSrc(), ev->getDest(),
                              id, getName().c_str(), i, progress_vcs[i], ev->getNextPort(), ev->getVC());
            }
        }
        else if ( progress_vcs[i] == -2 ) {
            if ( i < num_ports ) xbar_stalls[i]->addData(1);
            else if ( service->synthetic_stall ) service->synthetic_stall->addData(1);
        }

        // Should stop at zero, need to find a clean way to do this
        // with no branch.  For now it should work.
        if ( in_port_busy[i] != 0 ) in_port_busy[i]--;
        if ( i < num_ports && out_port_busy[i] != 0 ) out_port_busy[i]--;
    }

    return false;
}

void hr_router::setup()
{
    for ( int i = 0; i < num_ports; i++ ) {
    	ports[i]->setup();
    }
    if ( network_service && !network_service->processor->validateInstalledTransport() ) {
        merlin_abort.fatal(CALL_INFO, 1,
            "Network-service processor transport is unsupported by initialized downstream credits\n");
    }
}

void hr_router::finish()
{
    for ( int i = 0; i < num_ports; i++ ) {
    	ports[i]->finish();
    }

}

void
hr_router::init(unsigned int phase)
{
    for ( int i = 0; i < num_ports; i++ ) {
        ports[i]->init(phase);
        Event *ev = NULL;
        while ( (ev = ports[i]->recvUntimedData()) != NULL ) {
            internal_router_event *ire = dynamic_cast<internal_router_event*>(ev);
            if ( ire == NULL ) {
                ire = topo->process_UntimedData_input(static_cast<RtrEvent*>(ev));
            }
            std::vector<int> outPorts;
            topo->routeUntimedData(i, ire, outPorts);
            for ( std::vector<int>::iterator j = outPorts.begin() ; j != outPorts.end() ; ++j ) {
                /* Little tricky here.  Need to clone both the event, and the
                 * encapsulated event.
                 */
                switch ( topo->getPortState(*j) ) {
                case Topology::R2N:
                    ports[*j]->sendUntimedData(ire->getEncapsulatedEvent()->clone());
                    break;
                case Topology::R2R:
                // Ignore failed links during init
                case Topology::FAILED: {
                    internal_router_event *new_ire = ire->clone();
                    ports[*j]->sendUntimedData(new_ire);
                    break;
                }
                default:
                    break;
                }
            }
            delete ire;
        }
    }


    // Always do the above.  A few specific things to do during init

    // After phase 1, all the PortControl blocks will have reported
    // the requested VNs.  Now we need to translate this to the number
    // of VCs needed.
    // if ( phase == 1 ) {
    //     num_vcs = topo->computeNumVCs(requested_vns);
    //     init_vcs();
    // }

}

void
hr_router::complete(unsigned int phase)
{
    for ( int i = 0; i < num_ports; i++ ) {
        ports[i]->complete(phase);
        Event *ev = NULL;
        while ( (ev = ports[i]->recvUntimedData()) != NULL ) {
            internal_router_event *ire = dynamic_cast<internal_router_event*>(ev);
            if ( ire == NULL ) {
                ire = topo->process_UntimedData_input(static_cast<RtrEvent*>(ev));
            }
            std::vector<int> outPorts;
            topo->routeUntimedData(i, ire, outPorts);
            for ( std::vector<int>::iterator j = outPorts.begin() ; j != outPorts.end() ; ++j ) {
                /* Little tricky here.  Need to clone both the event, and the
                 * encapsulated event.
                 */
                switch ( topo->getPortState(*j) ) {
                case Topology::R2N:
                    ports[*j]->sendUntimedData(ire->getEncapsulatedEvent()->clone());
                    break;
                case Topology::R2R:
                // Ignore failed links during init
                case Topology::FAILED: {
                    internal_router_event *new_ire = ire->clone();
                    ports[*j]->sendUntimedData(new_ire);
                    break;
                }
                default:
                    break;
                }
            }
            delete ire;
        }
    }
}

void
hr_router::sendCtrlEvent(CtrlRtrEvent* ev, int port) {
    if ( port == -1 ) {
        // Need to route packet
        port = topo->routeControlPacket(ev);
    }

    if ( port >= num_ports ) {
        auto dest = ev->getDest();
    }
    // Event just gets forwarded to appropriate PortControl object
    ports[port]->sendCtrlEvent(ev);
}

void
hr_router::recvCtrlEvent(int port, CtrlRtrEvent* ev) {
    // Check to see what type of event it is
    switch ( ev->getCtrlType() ) {
    case CtrlRtrEvent::TOPOLOGY:
        // Event just gets sent on to topolgy object
        topo->recvTopologyEvent(port,static_cast<TopologyEvent*>(ev));
        break;
    default:
        // Route the ctrl event
        const auto& dest = ev->getDest();
        int port = topo->routeControlPacket(ev);
        if ( port == -1 ) {
            // Destined for this router
            if ( dest.addr_is_router ) {
                // Packet was sent to me, but I don't know what to do with
                // it
                fatal(CALL_INFO_LONG,-1,"ERROR: router %d received unknown ctrl event\n",id);
            }
            else {
                auto d = topo->getDeliveryPortForEndpointID(dest.addr);
                ports[d.second]->recvCtrlEvent(ev);
            }
        }
        else {
            int port = topo->routeControlPacket(ev);
            ports[port]->sendCtrlEvent(ev);
        }
        break;
    }
}


void
hr_router::init_vcs()
{
    vc_heads = new internal_router_event*[num_ports*num_vcs];
    xbar_in_credits = new int[num_ports*num_vcs];
    output_queue_lengths = new int[num_ports*num_vcs];
    for ( int i = 0; i < num_ports*num_vcs; i++ ) {
        vc_heads[i] = NULL;
        xbar_in_credits[i] = 0;
        output_queue_lengths[i] = 0;
    }

    for ( int i = 0; i < num_ports; i++ ) {
        ports[i]->initVCs(num_vns,vcs_per_vn.data(),&vc_heads[i*num_vcs],&xbar_in_credits[i*num_vcs],&output_queue_lengths[i*num_vcs]);
    }

    topo->setOutputBufferCreditArray(xbar_in_credits, num_vcs);
    topo->setOutputQueueLengthsArray(output_queue_lengths, num_vcs);

    // Now that we have the number of VCs we can finish initializing
    // arbitration logic
    if ( network_service ) {
        if ( !arb->setNetworkServiceInputs(numXbarInputs(), num_ports, num_vcs, network_service->owned_vc_mask) ) {
            merlin_abort.fatal(CALL_INFO, 1,
                "Configured crossbar arbiter does not support active network services; use merlin.xbar_arb_lru\n");
        }
    }
    else {
        arb->setPorts(num_ports,num_vcs);
    }


}

void
hr_router::reportIncomingEvent(internal_router_event* ev)
{
    // If this is destined for this router, let appropriate
    // PortControl know
    auto dest = topo->getDeliveryPortForEndpointID(ev->getDest());
    if ( dest.first == id ) {
        ports[dest.second]->reportIncomingEvent(ev);
    }
}
