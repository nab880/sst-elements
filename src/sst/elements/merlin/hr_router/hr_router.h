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


#ifndef COMPONENTS_HR_ROUTER_HR_ROUTER_H
#define COMPONENTS_HR_ROUTER_HR_ROUTER_H

#include <sst/core/clock.h>
#include <sst/core/component.h>
#include <sst/core/event.h>
#include <sst/core/link.h>
#include <sst/core/output.h>
#include <sst/core/timeConverter.h>

#include <sst/core/statapi/stataccumulator.h>
#include <sst/core/shared/sharedArray.h>

#include <queue>
#include <memory>
#include <vector>

#include "../router.h"

using namespace SST;

namespace SST {
namespace Merlin {

class PortControlBase;

class NetworkServiceSyntheticRequester : public XbarInput {
public:
    NetworkServiceSyntheticRequester(int num_vcs, uint32_t capacity);
    ~NetworkServiceSyntheticRequester() override;

    bool enqueue(std::unique_ptr<internal_router_event>& event, int vc);
    bool canEnqueue(int vc) const;
    bool hasWork() const { return size_ != 0; }
    uint32_t size() const { return size_; }

    internal_router_event* recv(int vc) override;
    internal_router_event** getVCHeads() override { return heads_.data(); }

    void serialize_order(SST::Core::Serialization::serializer& ser);

private:
    int num_vcs_ = 0;
    uint32_t capacity_ = 0;
    uint32_t size_ = 0;
    std::vector<std::queue<internal_router_event*>> queues_;
    std::vector<internal_router_event*> heads_;
};

/** Enabled-only adapter; public PortInterface keeps its released object layout. */
class NetworkServicePortXbarInput final : public XbarInput {
public:
    explicit NetworkServicePortXbarInput(PortInterface* port) : port_(port) {}

    internal_router_event* recv(int vc) override { return port_->recv(vc); }
    internal_router_event** getVCHeads() override { return port_->getVCHeads(); }

private:
    PortInterface* port_;
};

/** All state absent from routers without an installed network service. */
struct NetworkServiceRouterContext {
    NetworkServiceProcessor* processor = nullptr; // framework-owned
    std::unique_ptr<NetworkServiceSyntheticRequester> requester;
    uint32_t output_queue_depth = 0;
    uint64_t scan_cursor = 0;
    uint64_t tagged_heads = 0;

    std::vector<std::unique_ptr<NetworkServicePortXbarInput>> port_inputs;
    std::vector<XbarInput*> xbar_inputs;

    Statistic<uint64_t>* pass = nullptr;
    Statistic<uint64_t>* accept = nullptr;
    Statistic<uint64_t>* busy = nullptr;
    Statistic<uint64_t>* reject = nullptr;
    Statistic<uint64_t>* synthetic = nullptr;
    Statistic<uint64_t>* synthetic_stall = nullptr;

    void rebuildInputs(PortInterface** ports, int num_ports)
    {
        port_inputs.clear();
        xbar_inputs.clear();
        port_inputs.reserve(static_cast<size_t>(num_ports));
        xbar_inputs.reserve(static_cast<size_t>(num_ports) + 1);
        for ( int i = 0; i < num_ports; ++i ) {
            port_inputs.emplace_back(new NetworkServicePortXbarInput(ports[i]));
            xbar_inputs.push_back(port_inputs.back().get());
        }
        xbar_inputs.push_back(requester.get());
    }
};

class hr_router : public Router, public NetworkServiceHost {

public:

    SST_ELI_REGISTER_COMPONENT(
        hr_router,
        "merlin",
        "hr_router",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "High radix router",
        COMPONENT_CATEGORY_NETWORK)

    SST_ELI_DOCUMENT_PARAMS(
        {"id",                 "ID of the router."},
        {"num_ports",          "Number of ports that the router has"},
        {"topology",           "Name of the topology subcomponent that should be loaded to control routing."},
        {"xbar_arb",           "Arbitration unit to be used for crossbar. Defaults to LRU without a network service and service-capable RR with one.",""},
        {"link_bw",            "Bandwidth of the links specified in either b/s or B/s (can include SI prefix)."},
        {"flit_size",          "Flit size specified in either b or B (can include SI prefix)."},
        {"xbar_bw",            "Bandwidth of the crossbar specified in either b/s or B/s (can include SI prefix)."},
        {"input_latency",      "Latency of packets entering switch into input buffers.  Specified in s (can include SI prefix)."},
        {"output_latency",     "Latency of packets exiting switch from output buffers.  Specified in s (can include SI prefix)."},
        {"input_buf_size",     "Size of input buffers specified in b or B (can include SI prefix)."},
        {"output_buf_size",    "Size of output buffers specified in b or B (can include SI prefix)."},
        {"network_inspectors", "Comma separated list of network inspectors to put on output ports.", ""},
        {"oql_track_port",     "Set to true to track output queue length for an entire port.  False tracks per VC.", "false"},
        {"oql_track_remote",   "Set to true to track output queue length including remote input queue.  False tracks only local queue.", "false"},
        {"num_vns",            "Number of VNs.","2"},
        {"vn_remap",           "Array that specifies the vn remapping for each node in the systsm."},
        {"vn_remap_shm",       "Name of shared memory region for vn remapping.  If empty, no remapping is done", ""},
        {"debug",              "Turn on debugging for router. Set to 1 for on, 0 for off.", "0"},
        {"network_service_output_queue_depth", "Maximum synthetic packets retained by the optional network service processor.", "8"}
    )

    SST_ELI_DOCUMENT_STATISTICS(
        { "send_bit_count",     "Count number of bits sent on link", "bits", 1},
        { "send_packet_count",  "Count number of packets sent on link", "packets", 1},
        { "output_port_stalls", "Time output port is stalled (in units of core timebase)", "time in stalls", 1},
        { "xbar_stalls",        "Count number of cycles the xbar is stalled", "cycles", 1},
        { "idle_time",          "Amount of time spent idle for a given port", "units of core timebase", 1},
        { "width_adj_count",    "Number of times that link width was increased or decreased", "width adjustment count", 1},
        {"network_service_pass", "Tagged heads passed to ordinary routing", "packets", 1},
        {"network_service_accept", "Tagged heads consumed by a service processor", "packets", 1},
        {"network_service_busy", "Tagged heads retained because a service processor was busy", "packets", 1},
        {"network_service_reject", "Tagged heads rejected before terminal failure", "packets", 1},
        {"network_service_synthetic", "Synthetic packets granted through the crossbar", "packets", 1},
        {"network_service_synthetic_stall", "Cycles a synthetic head was unable to progress", "cycles", 1}
    )

    SST_ELI_DOCUMENT_PORTS(
        {"port%(num_ports)d",  "Ports which connect to endpoints or other routers.", { "merlin.RtrEvent", "merlin.internal_router_event", "merlin.topologyevent", "merlin.credit_event" } }
    )

    SST_ELI_DOCUMENT_SUBCOMPONENT_SLOTS(
        {"topology", "Topology object to control routing", "SST::Merlin::Topology" },
        {"XbarArb", "Crossbar arbitration", "SST::Merlin::XbarArbitration" },
        {"portcontrol", "PortControl blocks", "SST::Merlin::PortInterface" },
        {"network_service", "Optional generic packet service processor", "SST::Merlin::NetworkServiceProcessor" }
    )

private:
    int id;
    int num_ports;
    int num_vns;
    std::string vn_remap_shm;
    int vn_remap_shm_size;
    int num_vcs;
    int flit_size_bits;
    std::vector<int> vcs_per_vn;

    Topology* topo;
    XbarArbitration* arb;
    PortInterface** ports;
    std::unique_ptr<NetworkServiceRouterContext> network_service;
    internal_router_event** vc_heads;
    int* xbar_in_credits;
    int* output_queue_lengths;

#if VERIFY_DECLOCKING
    bool clocking;
#endif

    int* in_port_busy;
    int* out_port_busy;
    int* progress_vcs;

    UnitAlgebra input_buf_size;
    UnitAlgebra output_buf_size;

    Cycle_t unclocked_cycle;
    std::string xbar_bw;
    TimeConverter xbar_tc;
    Clock::HandlerBase* my_clock_handler;

    std::vector<std::string> inspector_names;

    bool clock_handler(Cycle_t cycle);
    bool clock_handler_no_service(Cycle_t cycle);
    bool clock_handler_with_service(Cycle_t cycle);
    int numXbarInputs() const { return num_ports + (network_service ? 1 : 0); }
    static void sigHandler(int signal);

    void init_vcs();
    Statistic<uint64_t>** xbar_stalls;

    Output& output;

    Shared::SharedArray<int> shared_array;

    SST_ELI_IS_CHECKPOINTABLE()

public:
    hr_router(ComponentId_t cid, Params& params);
    hr_router();
    ~hr_router();

    void init(unsigned int phase) override;
    void complete(unsigned int phase) override;
    void setup() override;
    void finish() override;

    void notifyEvent() override;
    int const* getOutputBufferCredits() override {return xbar_in_credits;}
    int const* getOutputQueueLengths() {return output_queue_lengths;}

    void sendCtrlEvent(CtrlRtrEvent* ev, int port = -1) override;
    void recvCtrlEvent(int port, CtrlRtrEvent* ev) override;

    void dumpState(std::ostream& stream);
    void printStatus(Output& out) override;

    void reportIncomingEvent(internal_router_event* ev) override;
    NetworkServiceID getNetworkServiceID() const override;
    NetworkServiceRequestContract getNetworkServiceRequestContract() const override;
    void networkServiceHeadAppeared() override;
    void networkServiceHeadRemoved() override;

    void serialize_order(SST::Core::Serialization::serializer& ser) override;
    ImplementSerializable(SST::Merlin::hr_router)

    bool tryEnqueueNetworkServiceOutput(
        NetworkServiceID service_id, NetworkServiceSyntheticPacket& packet) override;
    void wakeNetworkServiceProcessor() override;
};

}
}

#endif // COMPONENTS_HR_ROUTER_HR_ROUTER_H
