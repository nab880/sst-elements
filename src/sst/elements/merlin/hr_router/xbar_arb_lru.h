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


#ifndef COMPONENTS_HR_ROUTER_XBAR_ARB_LRU_H
#define COMPONENTS_HR_ROUTER_XBAR_ARB_LRU_H

#include <sst/core/component.h>
#include <sst/core/event.h>
#include <sst/core/link.h>
#include <sst/core/timeConverter.h>

#include <vector>

#include "sst/elements/merlin/router.h"

namespace SST {
namespace Merlin {

class xbar_arb_lru : public XbarArbitration {

public:

    SST_ELI_REGISTER_SUBCOMPONENT(
        xbar_arb_lru,
        "merlin",
        "xbar_arb_lru",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Least recently used arbitration unit for hr_router",
        SST::Merlin::XbarArbitration
    )


private:
    int num_ports = 0;
    int num_output_ports = 0;
    int num_vcs = 0;
    std::vector<uint8_t> owned_vcs;

#if VERIFY_DECLOCKING
    int rr_port_shadow;
#endif

    typedef std::pair<uint16_t,uint16_t> priority_entry_t;
    priority_entry_t* priority[2] = { nullptr, nullptr };
    priority_entry_t* cur_list = nullptr;
    priority_entry_t* next_list = nullptr;

    int total_entries = 0;



    // PortControl** ports;

public:

    xbar_arb_lru() : XbarArbitration() {}

    xbar_arb_lru(ComponentId_t cid, Params& param) :
        XbarArbitration(cid)
    {
    }

    ~xbar_arb_lru() {
        delete[] priority[0];
        delete[] priority[1];
    }

    void serialize_order(SST::Core::Serialization::serializer& ser) override {
        XbarArbitration::serialize_order(ser);
        SST_SER(num_ports);
        SST_SER(num_output_ports);
        SST_SER(owned_vcs);
        SST_SER(num_vcs);
        SST_SER(total_entries);

        // Need to serialize each of the priority arrays separately as there is no way to pass the fact that that the
        // data in the array are c-style arrays
        SST_SER(SST::Core::Serialization::array(priority[0], total_entries));
        SST_SER(SST::Core::Serialization::array(priority[1], total_entries));

        // Serialize which buffer is current vs next (as index 0 or 1)
        int cur_idx = (cur_list == priority[0]) ? 0 : 1;
        SST_SER(cur_idx);
        if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
            cur_list = priority[cur_idx];
            next_list = priority[1 - cur_idx];
        }
    }
    ImplementSerializable(SST::Merlin::xbar_arb_lru)

    void setPorts(int num_ports_s, int num_vcs_s) override
    {
        num_ports = num_ports_s;
        num_output_ports = num_ports_s;
        owned_vcs.clear();
        delete[] priority[0];
        delete[] priority[1];
        num_vcs = num_vcs_s;

        total_entries = num_ports * num_vcs;

        priority[0] = new priority_entry_t[total_entries];
        priority[1] = new priority_entry_t[total_entries];
        cur_list = priority[0];
        next_list = priority[1];

        int index = 0;
        for ( int i = 0; i < num_ports; i++ ) {
            for ( int j = 0; j < num_vcs; j++ ) {
                cur_list[index] = next_list[index] = priority_entry_t(i,j);
                ++index;
            }
        }



    }

    bool setNetworkServiceInputs(int num_inputs, int num_outputs, int vc_count,
        const std::vector<uint8_t>& owned) override
    {
        if ( num_inputs != num_outputs + 1 || num_outputs <= 0 || vc_count <= 0 ||
             owned.size() != static_cast<size_t>(vc_count) ) return false;
        setPorts(num_inputs, vc_count);
        num_output_ports = num_outputs;
        owned_vcs = owned;
        return true;
    }

#if VERIFY_DECLOCKING
    bool arbitrateNetworkService(XbarInput** inputs, PortInterface** outputs, int* input_busy,
        int* output_busy, int* progress_vc, bool clocking) override
#else
    bool arbitrateNetworkService(XbarInput** inputs, PortInterface** outputs, int* input_busy,
        int* output_busy, int* progress_vc) override
#endif
    {
        return arbitrateInputs(inputs, outputs, input_busy, output_busy, progress_vc);
    }

    void arbitrate(
#if VERIFY_DECLOCKING
        PortInterface** ports, int* input_busy, int* output_busy, int* progress_vc, bool clocking
#else
        PortInterface** ports, int* input_busy, int* output_busy, int* progress_vc
#endif
    ) override
    {
        arbitrateInputs(ports, ports, input_busy, output_busy, progress_vc);
    }

private:
    // One LRU policy for physical and synthetic inputs.  Empty synthetic
    // entries never change the relative priority of ordinary requests.
    template <class Input>
    bool arbitrateInputs(Input** inputs, PortInterface** outputs, int* input_busy,
        int* output_busy, int* progress_vc)
    {
        for ( int port = 0; port < num_ports; ++port ) progress_vc[port] = -1;
        priority_entry_t* satisfied = &next_list[total_entries - 1];
        priority_entry_t* waiting = next_list;
        for ( int i = 0; i < total_entries; ++i ) {
            const priority_entry_t entry = cur_list[i];
            const int port = entry.first;
            const int vc = entry.second;
            auto* event = inputs[port]->getVCHeads()[vc];
            const bool processor_owned = !owned_vcs.empty() && port < num_output_ports && owned_vcs[vc];
            if ( input_busy[port] <= 0 && event != nullptr && !processor_owned ) {
                const int output = event->getNextPort();
                const int output_vc = event->getVC();
                if ( output < 0 || output >= num_output_ports || output_vc < 0 || output_vc >= num_vcs ) {
                    return false;
                }
                if ( output_busy[output] <= 0 && outputs[output]->spaceToSend(output_vc, event->getFlitCount()) ) {
                    progress_vc[port] = vc;
                    input_busy[port] = output_busy[output] = event->getFlitCount();
                    *satisfied-- = entry;
                    continue;
                }
                progress_vc[port] = -2;
            }
            *waiting++ = entry;
        }
        std::swap(cur_list, next_list);
        return true;
    }

public:
    void reportSkippedCycles(Cycle_t cycles) override
    {}

    void dumpState(std::ostream& stream) override
    {
        /* stream << "Current round robin port: " << rr_port << std::endl; */
        /* stream << "  Current round robin VC by port:" << std::endl; */
        /* for ( int i = 0; i < num_ports; i++ ) { */
        /*     stream << i << ": " << rr_vcs[i] << std::endl; */
        /* } */
    }

};

}
}

#endif // COMPONENTS_HR_ROUTER_XBAR_ARB_LRU_H
